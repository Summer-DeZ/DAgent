/// @file compaction.hpp
/// @brief 上下文预算与压缩：在副本上计算候选变化，成功且未取消后由 SessionCommitter 一次性提交。
///
/// 使用工具事实保留、safe_cuts、摘要前缀和退化策略；Compactor 不直接修改 Conversation 或写记录。
#pragma once

#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "agent/conversation.hpp"
#include "agent/events.hpp"
#include "agent/options.hpp"
#include "agent/port_model.hpp"
#include "agent/tokens.hpp"

namespace dagent::agent {

struct RequestShape {
    std::string system;
    std::vector<ToolSpec> tools;
    ModelParams params; ///< 中立模型参数（model / max_tokens / temperature）
};

struct Budget {
    std::size_t limit, trigger, target;
    static Budget from(const ContextOptions&, std::size_t max_tokens);
};

namespace texts {
std::string pruned_output(std::string_view summary);
std::string summary_message(std::string_view summary);
} // namespace texts

/// @brief 一次压缩的候选结果：内存安装与记录写入都由提交器完成。
struct CompactionChange {
    Conversation conversation;            ///< 候选结果，直接替换会话历史
    std::vector<std::int64_t> pruned;     ///< 需要写 prune 的 tool ordinal
    std::int64_t keep_from = -1;          ///< 需要写 compaction 的切点；<0 表示无摘要记录
    std::string summary;                  ///< 空表示原有丢弃前缀退化
    std::size_t discarded = 0;            ///< 摘要失败时丢弃的条数，用于原 Notice
    std::size_t before = 0, after = 0;    ///< 估算 tokens，日志与 Compacted 事件用
    std::size_t limit = 0;                ///< 本轮上下文预算上限
    std::string_view mode_label;          ///< 日志用：自动/强制/手动
    bool summarized = false;
};

class Compactor {
public:
    Compactor(ContextOptions, std::size_t max_tokens, std::string compact_prompt);

    /// @brief 自动压缩：低于触发阈值返回 nullopt；取消/超窗抛原 ModelError。
    std::optional<CompactionChange> maybe_compact(const Conversation&, const RequestShape&, ModelSession&,
                                                  TokenEstimator&, const Sink&, std::stop_token);
    /// @brief 服务端报上下文超长后的强制压缩。
    std::optional<CompactionChange> force(const Conversation&, const RequestShape&, ModelSession&,
                                          TokenEstimator&, const Sink&, std::stop_token);
    /// @brief 手动 /compact：不追加 user/turn_end。
    std::optional<CompactionChange> summarize(const Conversation&, const RequestShape&, ModelSession&,
                                              TokenEstimator&, const Sink&, std::stop_token);
    Budget budget() const { return budget_; }

private:
    enum class Mode { automatic, forced, manual };
    std::optional<CompactionChange> compact(Mode, const Conversation&, const RequestShape&, ModelSession&,
                                            TokenEstimator&, const Sink&, std::stop_token);

    Budget budget_;
    std::string prompt_;
};

} // namespace dagent::agent
