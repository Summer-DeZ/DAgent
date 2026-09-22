/// @file conversation.hpp
/// @brief 发给模型的消息历史：条目、协议不变式、组装 Request、压缩用的切点与替换。
///
/// 纯内存数据结构：不做 I/O、不发事件、不落盘（docs/design/agent.md §10）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/message.hpp"

namespace dagent::agent {

struct Entry {
    Message message;
    std::int64_t ordinal = -1; ///< 会话记录里的序号（docs/design/agent.md §10）；压缩摘要是 -1
    bool pruned = false;       ///< tool 消息的内容已被裁剪成占位（docs/design/agent.md §8）
    std::string summary;       ///< tool 消息：调度时的 Intent::summary，生成裁剪占位用
    std::size_t tokens = 0;    ///< estimate_tokens 的缓存，加入或修改时算一次
};

/// @brief 核心自己生成、会进入历史给模型看的文字（T1–T9、T12 / T13；T10 / T11 见 compaction.hpp）。
namespace texts {
inline constexpr std::string_view kInterrupted = "\n\n[response interrupted by the user]";
inline constexpr std::string_view kInterruptedCall = "The user interrupted this turn; this call was not executed.";
inline constexpr std::string_view kDenied =
    "The user denied this call. Do not work around it; wait for further instructions.";
inline constexpr std::string_view kDeniedWithFeedback = "The user denied this call and said: {}";
inline constexpr std::string_view kPriorDenied = "An earlier call in the same batch was denied; this call was not executed.";
inline constexpr std::string_view kPolicyDenied =
    "The permission policy denied this call: {}. Use an approach that does not need this permission, "
    "or say in your final reply what the user must do.";
inline constexpr std::string_view kApprovalUnavailable =
    "This call requires user approval: {}. This run has no interactive approver, so the call was not executed. "
    "Retry it in an interactive session to approve once, or explicitly use unrestricted mode if full host access is intended.";
inline constexpr std::string_view kUnknownTool = "Unknown tool {}. Available tools: {}";
inline constexpr std::string_view kToolLimit =
    "This turn hit the tool call limit ({} calls); this call was not executed. Summarize what you finished and tell the user what is left.";
inline constexpr std::string_view kMcpReconnecting =
    "\n\nMCP server {} disconnected. It reconnects once before the next step; its tools come back if that succeeds.";
inline constexpr std::string_view kMcpUnavailable =
    "\n\nMCP server {} is unavailable and will not be retried this session; its tools were removed. Do not call them - use another approach or tell the user.";
inline constexpr std::string_view kCrashed =
    "The session was interrupted while this call was running, so the result is unknown. If it may have changed files or state, check the current state before continuing.";
} // namespace texts

class Conversation {
public:
    // ---- 追加（每个都返回新条目的 ordinal）----
    std::int64_t add_user(std::string text);
    std::int64_t add_assistant(Message message); ///< 可带 tool_calls；进入打开状态
    std::int64_t add_tool_result(std::string_view call_id, std::string text, std::string summary);

    /// @brief 当前打开的 assistant 里还没有 tool 消息的调用，按 tool_calls 顺序。闭合时为空。
    std::vector<ToolCall> open_calls() const;

    // ---- 读取 ----
    const std::deque<Entry>& entries() const { return entries_; }
    std::size_t tokens() const;
    std::int64_t next_ordinal() const { return next_ordinal_; }

    /// @brief system 放最前面，其后是全部 entries 的 message。要求闭合。
    Request build(const std::string& system, const std::vector<ToolSpec>& tools,
                  const ModelParams& params) const;

    /// @brief 检查 I1–I4；返回第一条违反的描述。debug 构建里 build 开头 assert 它为空。
    std::optional<std::string> validate() const;

    // ---- 压缩（docs/design/agent.md §8）----
    std::vector<std::size_t> safe_cuts() const; ///< 可以切开的下标，升序
    void prune(std::size_t tool_entry, std::string placeholder);
    void replace_prefix(std::size_t cut, std::string summary_message);
    void discard_prefix(std::size_t cut); ///< 保留已有摘要；调用方保证尾部满足 I4

    // ---- 恢复（docs/design/agent.md §10）----
    void restore(Entry entry); ///< 按记录原样放回，不重新分配 ordinal
    void set_next_ordinal(std::int64_t);

private:
    std::size_t compute_tokens(const Message&) const;

    std::deque<Entry> entries_;
    std::int64_t next_ordinal_ = 0;
};

} // namespace dagent::agent
