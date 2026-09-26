/// @file committer.hpp
/// @brief SessionCommitter：会话内存状态、记录与通知的唯一提交入口。
///
/// 统一完成会话内存更新、记录追加与事件通知；
/// broken/error 由提交器唯一持有，第一次写入失败后继续当前回合并只通知一次。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agent/conversation.hpp"
#include "agent/events.hpp"
#include "agent/port_journal.hpp"
#include "agent/tool_data.hpp"
#include "agent/work_plan.hpp"

namespace dagent::agent {

struct CompactionChange; // compaction.hpp

class SessionCommitter {
public:
    SessionCommitter(Conversation& conversation, WorkPlan& plan, JournalWriter& journal)
        : conversation_(conversation), plan_(plan), journal_(journal) {}
    SessionCommitter(const SessionCommitter&) = delete;
    SessionCommitter& operator=(const SessionCommitter&) = delete;

    /// @brief 绑定本轮 Sink（broken 通知与完成事件用）；Run 结束时清除。
    void set_sink(const Sink& sink) { sink_ = &sink; }
    void clear_sink() { sink_ = nullptr; }

    // ---- live_message ----
    std::int64_t commit_user(std::string text);
    std::int64_t commit_assistant(const Reply& reply);
    /// @brief 按原序提交一个工具结果；todo 的 WorkPlan 替换与 tool 记录在同一提交内完成。
    std::int64_t commit_tool(const ToolCall& call, std::string_view summary, const ToolResult& result,
                             const TodoView* plan = nullptr);

    // ---- partial_response ----
    std::int64_t commit_partial(std::string_view content);

    // ---- execution_audit ----
    void commit_tool_started(const ToolStarted& event);
    void commit_permission(const Approval& approval, const Decision& decision);
    void commit_permission_revoked(std::string_view id);

    // ---- compact_commit ----
    void commit_compaction(CompactionChange change);

    // ---- turn_repair / recovery_repair ----
    /// @brief 收尾补调用：内存与记录，不新增实时 ToolFinished。
    void repair_open_calls();
    /// @brief 显式恢复补闭合：tool 记录 + crashed turn_end + sync，并把历史事件交给 sink。
    void repair_crashed_calls(const std::vector<ToolCall>& open_calls, const Sink& replay_sink);

    // ---- lifecycle_record ----
    void record_system(std::string_view text, std::string_view model);
    void record_turn_end(TurnStatus status, std::string_view error, int steps, int tool_calls,
                         const Usage& total);
    void record_turn_end_crashed();
    void sync();

    bool broken() const { return broken_; }
    const std::string& error() const { return error_; }
    /// @brief 首次写入失败后只发一次 Notice；每个记录写入点按原顺序调用。
    void check_broken();

private:
    void append(const Record& record);
    void fail(const std::string& what);

    Conversation& conversation_;
    WorkPlan& plan_;
    JournalWriter& journal_;
    const Sink* sink_ = nullptr;
    bool broken_ = false;
    bool notified_ = false;
    std::string error_;
};

} // namespace dagent::agent
