/// @file conversation.hpp
/// @brief 发给模型的消息历史：条目、协议不变式、组装 Request、压缩用的切点与替换。
///
/// 纯内存数据结构：不做 I/O、不发事件、不落盘（09-record）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/message.hpp"
#include "agent/model.hpp"

namespace dagent::agent {

struct Entry {
    Message message;
    std::int64_t ordinal = -1; ///< 会话记录里的序号（09-record）；压缩摘要是 -1
    bool pruned = false;       ///< tool 消息的内容已被裁剪成占位（07-context）
    std::string summary;       ///< tool 消息：调度时的 Intent::summary，生成裁剪占位用
    std::size_t tokens = 0;    ///< estimate_tokens 的缓存，加入或修改时算一次
};

/// @brief 核心自己生成、会进入历史给模型看的文字（T1–T9；T10 / T11 见 compaction.hpp）。
namespace texts {
inline constexpr std::string_view kInterrupted = "\n\n[回复被用户中断]";
inline constexpr std::string_view kInterruptedCall = "用户中断了本轮，这个调用没有执行。";
inline constexpr std::string_view kDenied =
    "用户拒绝了这次调用。不要换一种方式绕过它，等待用户的进一步指示。";
inline constexpr std::string_view kDeniedWithFeedback = "用户拒绝了这次调用，并说明：{}";
inline constexpr std::string_view kPriorDenied = "同一批里前面的调用被用户拒绝，这个调用没有执行。";
inline constexpr std::string_view kPolicyDenied =
    "权限策略拒绝了这次调用：{}。当前是非交互模式，无法向用户确认；请换一种不需要这个权限的做法，"
    "或在最终回复里说明需要用户做什么。";
inline constexpr std::string_view kUnknownTool = "未知工具 {}。可用的工具：{}";
inline constexpr std::string_view kToolLimit =
    "本轮工具调用已达上限（{} 次），这个调用没有执行。请总结目前的进展，并告诉用户还有什么没做完。";
inline constexpr std::string_view kCrashed =
    "会话在执行这个调用时意外中断，结果未知。如果它会修改文件或状态，请先检查当前状态再继续。";
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
    Request build(const std::string& system, const std::vector<ToolDef>& tools, const ModelParams&) const;

    /// @brief 检查 I1–I4；返回第一条违反的描述。debug 构建里 build 开头 assert 它为空。
    std::optional<std::string> validate() const;

    // ---- 压缩（07-context）----
    std::vector<std::size_t> safe_cuts() const; ///< 可以切开的下标，升序
    void prune(std::size_t tool_entry, std::string placeholder);
    void replace_prefix(std::size_t cut, std::string summary_message);
    void discard_prefix(std::size_t cut); ///< 保留已有摘要；调用方保证尾部满足 I4

    // ---- 恢复（09-record）----
    void restore(Entry entry); ///< 按记录原样放回，不重新分配 ordinal
    void set_next_ordinal(std::int64_t);

private:
    std::size_t compute_tokens(const Message&) const;

    std::deque<Entry> entries_;
    std::int64_t next_ordinal_ = 0;
};

} // namespace dagent::agent
