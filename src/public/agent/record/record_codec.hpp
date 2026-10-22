/// @file record_codec.hpp
/// @brief RecordCodec：记录 payload 的唯一编解码入口。
///
/// 编码输出采用 events.type/payload 格式；解码产生类型化事实并校验当前字段形状，集中处理
/// 错误分类（corrupt）。恢复与历史投影共用本解码器，不各自手写字段解析。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agent/decision_types.hpp"
#include "agent/events.hpp"
#include "agent/message.hpp"
#include "agent/port/journal.hpp"
#include "agent/reply.hpp"
#include "agent/tool_data.hpp"
#include "lib/nlohmann/json.hpp"
namespace dagent::agent::record_codec {

// ---- 编码 ----

Record system(std::string_view text, std::string_view model);
Record user(std::int64_t n, std::string_view text, const std::vector<SkillView>& skills = {},
            UserInputOrigin origin = UserInputOrigin::user);
Record assistant(std::int64_t n, const Reply& reply);
Record tool_started(const ToolStarted& event);
Record tool(std::int64_t n, const ToolCall& call, std::string_view summary, const ToolResult& result);
Record permission(const Approval& approval, const Decision& decision,
                  const ExecutionGrant* grant = nullptr);
Record parent_review(const Approval& approval, const Decision& decision);
std::string permission_summary(const Approval& approval, const Decision& decision);
/// @brief 记录里的决定状态名（approved / denied 或显式 state）与授权范围名（none / one_call / session / child_session）。
std::string decision_state(const Decision& decision);
std::string decision_scope(const Approval& approval, const Decision& decision);
Record permission_revoked(std::string_view id);
Record auto_review(const AutoReview& event);
/// @brief 用户确认 unrestricted 警告的事实；schema 1，空载荷。
Record unrestricted_ack();
/// @brief 子会话初始化记录：主要任务与最多两条辅助用户原话；恢复时保留，不补造。
Record delegation_context(std::string_view task, const std::vector<std::string>& user_messages);
Record prune(const std::vector<std::int64_t>& ordinals);
Record compaction(std::int64_t keep_from, std::string_view summary);
Record turn_end(TurnStatus status, std::string_view error, int steps, int tool_calls, const Usage& total);
Record turn_end_crashed();

// ---- 解码：类型化事实 ----

struct SystemRecord {
    std::string text;
    std::string model;
};

struct UserRecord {
    std::int64_t n = 0;
    std::string text;
    std::vector<SkillView> skills; ///< explicit requests only; never restored as active guidance
    /// 缺失（旧记录）时按会话身份判定：主会话为 user，子会话为 delegation。
    std::optional<UserInputOrigin> origin;
};

struct AssistantRecord {
    std::int64_t n = 0;
    Message message;
    std::string finish;
    std::optional<Usage> usage;
};

struct ToolStartedRecord {
    ToolStarted event;
};

struct ToolRecord {
    std::int64_t n = 0;
    ToolCall call;
    std::string summary;
    ToolResult result;
};

struct PermissionRecord {
    std::string call_id, answer, rule;
    ApprovalAuthority authority = ApprovalAuthority::user;
    ApprovalIdentity identity;
    std::string state, scope, reason, model, tool;
    std::string approval_scope; ///< operation / exact_file_read / network_target / web_request
    std::vector<std::string> review_ids; ///< 关联的决策审阅记录
    Usage usage;
    nlohmann::json audit;
    std::string summary;
};

struct ParentReviewRecord { PermissionRecord review; };

struct AutoReviewRecord {
    std::string call_id, result, reason;
    std::vector<std::string> review_ids;
};

struct DelegationContextRecord {
    std::string task;
    std::vector<std::string> user_messages;
};

/// @brief 本会话用户确认 unrestricted 警告；旧会话缺失表示未确认。
struct UnrestrictedAckRecord {};

struct PermissionRevokedRecord {
    std::string id;
};

struct PruneRecord {
    std::vector<std::int64_t> ordinals;
};

struct CompactionRecord {
    std::int64_t keep_from = -1;
    std::string summary;
};

struct TurnEndRecord {
    TurnStatus status = TurnStatus::done;
    std::string error;
    int steps = 0, tool_calls = 0;
    Usage usage;
};

using DecodedRecord =
    std::variant<SystemRecord, UserRecord, AssistantRecord, ToolStartedRecord, ToolRecord,
                 PermissionRecord, ParentReviewRecord, AutoReviewRecord, DelegationContextRecord,
                 UnrestrictedAckRecord, PermissionRevokedRecord, PruneRecord, CompactionRecord,
                 TurnEndRecord>;

/// @brief 解码一条记录；未知类型或非法必须字段抛 RecordError(corrupt)。
DecodedRecord decode(std::string_view type, const nlohmann::json& payload);

} // namespace dagent::agent::record_codec
