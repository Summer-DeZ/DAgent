/// @file decision_types.hpp
/// @brief 决策审阅的中立值类型：场景、问句、分数、阈值、审阅结果与记录。
///
/// 核心只依赖 DecisionModel / DecisionReviewStore 端口与这些值；协议、存储实现留在外围。
/// 名词「决策模型」是第一版实际使用 d3 的通用叫法，换模型不改这里的场景与判定顺序。
#pragma once

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agent/grant.hpp"
#include "agent/intent.hpp"
#include "agent/reply.hpp"

namespace dagent::agent {

/// @brief 打分维度；键名只是标识，问句与选项说明见实现中的固定常量。
enum class Dimension { relevance, direction, net_change, privacy, injection };

/// @brief 数据流向类别（direction 的选项）。
enum class Direction { fetch, modify, send, unknown };

/// @brief 审阅场景：决定使用哪些维度、state 里放什么证据。
enum class ReviewScene {
    command_network, ///< 普通命令联网
    script_basic,    ///< 明确目标脚本的基础审查
    script_diff,     ///< 脚本 diff 复审
    outside_read,    ///< 工作区外普通文件读取（工具或简单命令）
    mcp_action,      ///< MCP 调用本身
    fetch_url,       ///< web_fetch 的逐跳完整 URL
    web_content,     ///< web_fetch 结果的注入扫描
};

std::string_view to_string(Dimension);
std::string_view to_string(Direction);
std::string_view to_string(ReviewScene);

/// @brief 审阅身份：阈值与反馈按它隔离；任一变化即不继承旧动态阈值。
struct ReviewVersion {
    std::string provider_kind;   ///< 协议 kind，如 systemone
    std::string adapter_version; ///< 适配器版本
    std::string endpoint;        ///< 完整服务地址
    std::string model;           ///< 模型标识，如 d3
    std::string model_revision;  ///< 部署方维护的权重/量化版本
    std::string question_version;///< 问句与选项说明的版本
    std::string threshold_profile; ///< 阈值配置名

    /// @brief 存储键：所有身份字段拼接。
    std::string key() const;
    friend bool operator==(const ReviewVersion&, const ReviewVersion&) = default;
};

/// @brief 一次请求里的一个问题：criteria 为空表示 noul（布尔概率）。
struct DecisionQuestion {
    Dimension dimension = Dimension::relevance;
    std::string instructions;
    std::vector<std::pair<Direction, std::string>> criteria; ///< choice 的类别与选项说明
};

/// @brief choice 的分数：选中类别与各类别概率。
struct DirectionScore {
    Direction selected = Direction::unknown;
    std::map<Direction, double> probabilities;
};

using DecisionScore = std::variant<double, DirectionScore>;

/// @brief 一次决策请求；adapter 只负责把它编码成自己的协议。
struct DecisionRequest {
    std::string state;
    std::vector<DecisionQuestion> questions;
};

/// @brief 一次决策响应；scores 只含本次请求的维度。
struct DecisionReply {
    std::string model;
    std::map<Dimension, DecisionScore> scores;
    Usage usage;
};

struct DecisionError {
    enum class Kind { unavailable, timeout, invalid_response, cancelled };
    Kind kind = Kind::unavailable;
    std::string detail;
};

/// @brief 一次审阅的截止时间（steady_clock）；从提交请求起计入排队与 HTTP 等待。
using ReviewDeadline = std::chrono::steady_clock::time_point;

/// @brief 单个维度的动态阈值：严格端 / 宽松端 / 当前值。
struct AdaptiveThreshold {
    enum class Pass { at_least, at_most };
    Pass pass = Pass::at_least;
    double strict_value = 0.0;
    double relaxed_value = 0.0;
    double current_value = 0.0;
};

/// @brief 阈值键：场景 + 维度 + 方向（fetch / send 分开）。
struct ThresholdKey {
    ReviewScene scene = ReviewScene::command_network;
    Dimension dimension = Dimension::relevance;
    std::optional<Direction> direction;

    friend bool operator<(const ThresholdKey& a, const ThresholdKey& b) {
        if (a.scene != b.scene) return a.scene < b.scene;
        if (a.dimension != b.dimension) return a.dimension < b.dimension;
        return static_cast<int>(a.direction.value_or(Direction::unknown)) <
               static_cast<int>(b.direction.value_or(Direction::unknown));
    }
    friend bool operator==(const ThresholdKey&, const ThresholdKey&) = default;
};

/// @brief 一个版本的阈值快照；缺少的键表示该场景尚未标定。
struct ThresholdProfile {
    ReviewVersion version;
    std::uint64_t revision = 0;
    std::map<ThresholdKey, AdaptiveThreshold> adaptive;
    /// 只供脚本 diff 分流使用，不参与动态调整。
    std::optional<double> net_change_low;
    std::optional<double> net_change_high;
    /// 对外注入扫描的固定阈值。
    std::optional<double> injection_limit;

    const AdaptiveThreshold* find(ReviewScene, Dimension,
                                  std::optional<Direction> = std::nullopt) const;
    const AdaptiveThreshold* find_direction(ReviewScene, Direction) const;
};

/// @brief 一条 user 消息的来源：真实用户输入或父 Agent 委派任务。
enum class UserInputOrigin { user, delegation };

/// @brief 审批背景：用户原话、委派任务、工作区与会话开始时间。
struct ReviewContext {
    std::vector<std::string> user_messages; ///< 主会话最多 3 条；子会话为辅助参考
    std::string delegated_task;             ///< 子会话的主要依据；主会话为空
    std::filesystem::path workspace_root;
    std::chrono::system_clock::time_point session_started{};
};

/// @brief 待审操作的确定性事实；state 的「待审批操作」段由它生成。
struct ReviewOperation {
    ApprovalIdentity identity; ///< 关联身份；审阅记录用它关联会话与执行
    std::string tool;      ///< 工具名（bash / read / web_fetch / mcp 工具名 / task）
    std::string command;   ///< 命令原文或 MCP 参数（截断只作用于提交副本）
    std::string arguments; ///< 原始工具参数
    std::optional<NetworkTarget> network; ///< 网络目标 host:port
    std::optional<std::filesystem::path> path; ///< 工作区外读取路径
    std::string url;       ///< web_fetch 当前完整 URL
    bool file_execution = false; ///< 是否文件执行
    std::string target;    ///< 当前审查目标（脚本路径）；无则为空
};

enum class ReviewDisposition { approve, defer, unavailable, cancelled };

/// @brief 未达标的单个检查；只在后端使用，供动态阈值归因。
struct FailedCheck {
    ReviewScene scene = ReviewScene::command_network;
    Dimension dimension = Dimension::relevance;
    std::optional<Direction> direction;
    double score = 0.0;
    bool adjustable = false;
};

struct ReviewResult {
    ReviewDisposition disposition = ReviewDisposition::unavailable;
    std::string record_id;
    std::string reason_code;
    std::string reason;
    std::vector<FailedCheck> failed_checks; ///< 未达标维度（已格式化，不外显分数）
};

/// @brief 一次执行级脚本证据：本会话未修改 / diff / 全文 / 超长 / 不可用。
struct ScriptEvidence {
    enum class Kind { unchanged, diff, full_text, too_large, unavailable };
    Kind kind = Kind::unavailable;
    std::filesystem::path target;
    std::string text;          ///< 仅此目标的 diff 或全文；unchanged 时为空
    std::size_t content_lines = 0;
    std::string reason;        ///< too_large / unavailable 的固定说明
};

/// @brief 一次决策请求的记录：实际 state、问句、响应与耗时。
struct DecisionAttempt {
    ReviewScene scene = ReviewScene::command_network;
    std::string target_id;
    DecisionRequest request;
    std::optional<DecisionReply> reply;
    std::optional<DecisionError> error;
    std::chrono::milliseconds elapsed{0};
};

/// @brief 一次逻辑审阅的完整记录（一个或多个 attempt）。
struct DecisionReviewRecord {
    std::string id;
    ApprovalIdentity identity;
    std::string root_turn_id;
    ReviewVersion version;
    ThresholdProfile thresholds;
    std::vector<DecisionAttempt> attempts;
    ReviewResult result;
};

/// @brief 人工最终决定回填：用于动态阈值与后续校准。
struct ReviewFeedback {
    std::string approval_request_id;
    std::vector<std::string> review_ids;
    ApprovalAuthority final_authority = ApprovalAuthority::user;
    bool user_decided = false; ///< 只有最终人工批准/拒绝才为 true
    bool approved = false;
};

/// @brief 整轮自动审批的关闭原因。
enum class AutoReviewBlock { none, injection, scan_failed };

/// @brief 本轮及所有子 Agent 共享的单向自动审批开关；关闭后不能被随后返回的正常结果恢复。
class RunReviewGate {
public:
    bool enabled() const { return reason_.load() == AutoReviewBlock::none; }
    AutoReviewBlock reason() const { return reason_.load(); }

    /// @brief 只允许从 none 转为关闭；返回是否是首次关闭。
    bool disable(AutoReviewBlock block) {
        if (block == AutoReviewBlock::none) return false;
        auto expected = AutoReviewBlock::none;
        return reason_.compare_exchange_strong(expected, block);
    }

private:
    std::atomic<AutoReviewBlock> reason_{AutoReviewBlock::none};
};

/// @brief web 结果抽样：切片范围用原始结果的 UTF-8 字节偏移表示。
struct ScanSlice {
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct ScanSample {
    std::string text;
    std::vector<ScanSlice> slices;
};

} // namespace dagent::agent
