#include "agent/committer.hpp"

#include <format>
#include <utility>

#include "agent/compaction.hpp"
#include "agent/record_codec.hpp"
#include "base/log.hpp"

namespace dagent::agent {
namespace {

std::shared_ptr<spdlog::logger> log_agent() { return base::logger("agent"); }

} // namespace

void SessionCommitter::append(const Record& record) {
    if (broken_) return;
    try {
        journal_.append(record);
    } catch (const RecordError& error) {
        fail(error.what());
    }
}

void SessionCommitter::fail(const std::string& what) {
    broken_ = true;
    error_ = what;
    log_agent()->error("failed to write session record: {}", what);
}

void SessionCommitter::check_broken() {
    if (!broken_ || notified_ || sink_ == nullptr) return;
    notified_ = true;
    (*sink_)(Notice{Notice::Level::error,
                    std::format("Failed to write the session record; subsequent content will not be saved: {}",
                                error_)});
}

std::int64_t SessionCommitter::commit_user(std::string text, const std::vector<SkillView>& skills) {
    const std::int64_t ordinal = conversation_.add_user(text);
    append(record_codec::user(ordinal, text, skills));
    check_broken();
    return ordinal;
}

std::int64_t SessionCommitter::commit_assistant(const Reply& reply) {
    const std::int64_t ordinal = conversation_.add_assistant(reply.message);
    append(record_codec::assistant(ordinal, reply));
    check_broken();
    return ordinal;
}

std::int64_t SessionCommitter::commit_tool(const ToolCall& call, std::string_view summary,
                                           const ToolResult& result, const TodoView* plan) {
    if (plan != nullptr) plan_.replace(*plan); // 计划替换与 tool 记录同一次提交
    const std::int64_t ordinal =
        conversation_.add_tool_result(call.id, result.model_text, std::string(summary));
    append(record_codec::tool(ordinal, call, summary, result));
    check_broken();
    if (sink_ != nullptr) {
        (*sink_)(ToolFinished{call.id, call.name, std::string(summary), result});
    }
    return ordinal;
}

std::int64_t SessionCommitter::commit_partial(std::string_view content) {
    if (content.empty()) return -1;

    Message message;
    message.role = Role::assistant;
    message.content = std::string(content) + std::string(texts::kInterrupted);
    Reply stored;
    stored.message = std::move(message);
    const std::int64_t ordinal = conversation_.add_assistant(stored.message);
    append(record_codec::assistant(ordinal, stored));
    check_broken();
    return ordinal;
}

void SessionCommitter::commit_tool_started(const ToolStarted& event) {
    append(record_codec::tool_started(event));
    check_broken();
}

void SessionCommitter::commit_permission(const Approval& approval, const Decision& decision) {
    append(record_codec::permission(approval, decision));
    check_broken();
}

void SessionCommitter::commit_permission_revoked(std::string_view id) {
    append(record_codec::permission_revoked(id));
}

void SessionCommitter::commit_compaction(CompactionChange change) {
    conversation_ = std::move(change.conversation);
    if (!change.pruned.empty()) append(record_codec::prune(change.pruned));
    if (change.keep_from >= 0) append(record_codec::compaction(change.keep_from, change.summary));
    log_agent()->info("上下文已压缩（{}）：{} → {} tokens，裁剪 {} 条，{}", change.mode_label,
                      change.before, change.after, change.pruned.size(),
                      !change.summary.empty() ? "已摘要"
                      : change.discarded > 0  ? std::format("摘要失败，丢弃 {} 条", change.discarded)
                                              : std::string("未摘要"));
    if (sink_ == nullptr) return;
    if (change.discarded > 0) {
        (*sink_)(Notice{Notice::Level::warn,
                        std::format("Summary failed; discarded the earliest {} messages", change.discarded)});
    }
    (*sink_)(Compacted{change.before, change.after, change.summarized});
    (*sink_)(ContextUpdate{{}, change.after, change.limit});
}

void SessionCommitter::repair_open_calls() {
    for (const ToolCall& call : conversation_.open_calls()) {
        const std::string text(texts::kInterruptedCall);
        const std::int64_t ordinal =
            conversation_.add_tool_result(call.id, text, "interrupted");
        ToolResult result;
        result.model_text = text;
        result.interrupted = true;
        append(record_codec::tool(ordinal, call, "interrupted", result));
    }
    check_broken();
}

void SessionCommitter::repair_crashed_calls(const std::vector<ToolCall>& open_calls,
                                            const Sink& replay_sink) {
    for (const ToolCall& call : open_calls) {
        const std::string text(texts::kCrashed);
        const std::string summary = "Recover interrupted " + call.name;
        const std::int64_t ordinal = conversation_.add_tool_result(call.id, text, summary);
        ToolResult result;
        result.model_text = text;
        result.is_error = true;
        result.interrupted = true;
        append(record_codec::tool(ordinal, call, summary, result));
        replay_sink(ToolFinished{call.id, call.name, summary, result});
    }
    record_turn_end_crashed();
    sync();
    replay_sink(TurnEnded{TurnStatus::failed, "session unexpectedly interrupted", 0, 0, {}});
}

void SessionCommitter::record_system(std::string_view text, std::string_view model) {
    append(record_codec::system(text, model));
}

void SessionCommitter::record_turn_end(TurnStatus status, std::string_view error, int steps,
                                       int tool_calls, const Usage& total) {
    append(record_codec::turn_end(status, error, steps, tool_calls, total));
}

void SessionCommitter::record_turn_end_crashed() {
    append(record_codec::turn_end_crashed());
}

void SessionCommitter::sync() {
    if (broken_) return;
    try {
        journal_.sync();
    } catch (const RecordError& error) {
        fail(error.what());
    }
}

} // namespace dagent::agent
