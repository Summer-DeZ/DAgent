#include "agent/recovery.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

#include "agent/compaction.hpp"
#include "agent/record_codec.hpp"

namespace dagent::agent {
namespace {

[[noreturn]] void corrupt(std::string_view what) {
    throw RecordError(RecordError::Kind::corrupt, std::format("corrupt session record: {}", what));
}

Entry entry_from(const Message& message, std::int64_t ordinal) {
    Entry entry;
    entry.message = message;
    entry.ordinal = ordinal;
    return entry;
}

} // namespace

RecoveryResult SessionRecovery::restore(const std::vector<StoredRecord>& records) const {
    RecoveryResult result;
    bool open_turn = false;
    std::int64_t next_ordinal = 0;
    bool saw_system = false;

    for (const StoredRecord& stored : records) {
        const auto decoded = record_codec::decode(stored.type, stored.payload);
        std::visit(
            [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, record_codec::SystemRecord>) {
                    if (!saw_system && stored.type != "system") corrupt("first core record is not system");
                    if (!value.model.empty()) result.model = value.model;
                    saw_system = true;
                } else if constexpr (std::is_same_v<T, record_codec::UserRecord>) {
                    if (!saw_system) corrupt("first core record is not system");
                    if (value.n != next_ordinal)
                        corrupt(std::format("expected message ordinal {}, got {}", next_ordinal, value.n));
                    Message message;
                    message.role = Role::user;
                    message.content = value.text;
                    result.conversation.restore(entry_from(message, value.n));
                    ++next_ordinal;
                    open_turn = true;
                } else if constexpr (std::is_same_v<T, record_codec::AssistantRecord>) {
                    if (!saw_system) corrupt("first core record is not system");
                    if (value.n != next_ordinal)
                        corrupt(std::format("expected message ordinal {}, got {}", next_ordinal, value.n));
                    result.conversation.restore(entry_from(value.message, value.n));
                    ++next_ordinal;
                } else if constexpr (std::is_same_v<T, record_codec::ToolRecord>) {
                    if (!saw_system) corrupt("first core record is not system");
                    if (value.n != next_ordinal)
                        corrupt(std::format("expected message ordinal {}, got {}", next_ordinal, value.n));
                    Message message;
                    message.role = Role::tool;
                    message.tool_call_id = value.call.id;
                    message.content = value.result.model_text;
                    Entry entry = entry_from(message, value.n);
                    entry.summary = value.summary;
                    result.conversation.restore(std::move(entry));
                    // 从工具结果中的 TodoView 重建 WorkPlan，不新增 Plan 记录。
                    if (const auto* plan = std::get_if<TodoView>(&value.result.display)) {
                        result.plan.replace(*plan);
                    }
                    ++next_ordinal;
                } else if constexpr (std::is_same_v<T, record_codec::PruneRecord>) {
                    for (const std::int64_t n : value.ordinals) {
                        const auto& entries = result.conversation.entries();
                        const auto found = std::find_if(entries.begin(), entries.end(),
                                                        [n](const Entry& e) { return e.ordinal == n; });
                        if (found == entries.end() || found->message.role != Role::tool) {
                            corrupt("pruned ordinal has no matching tool message");
                        }
                        result.conversation.prune(static_cast<std::size_t>(found - entries.begin()),
                                                  texts::pruned_output(found->summary));
                    }
                } else if constexpr (std::is_same_v<T, record_codec::CompactionRecord>) {
                    const auto& entries = result.conversation.entries();
                    const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) {
                        return e.ordinal == value.keep_from;
                    });
                    if (value.keep_from < 0 || found == entries.end())
                        corrupt("summary cut does not exist");
                    const auto cut = static_cast<std::size_t>(found - entries.begin());
                    const auto cuts = result.conversation.safe_cuts();
                    if (std::find(cuts.begin(), cuts.end(), cut) == cuts.end())
                        corrupt("summary cut is unsafe");
                    if (value.summary.empty()) result.conversation.discard_prefix(cut);
                    else result.conversation.replace_prefix(cut, texts::summary_message(value.summary));
                    if (const auto invalid = result.conversation.validate()) corrupt(*invalid);
                } else if constexpr (std::is_same_v<T, record_codec::TurnEndRecord>) {
                    open_turn = false;
                }
                // tool_started / permission / permission_revoked 只保留审计信息，不参与重建。
            },
            decoded);
    }

    if (!saw_system) {
        throw RecordError(RecordError::Kind::corrupt, "session record has no system entry");
    }
    result.conversation.set_next_ordinal(next_ordinal);
    result.unfinished = open_turn;

    // 未闭合历史只允许缺少最后一批工具结果：先在内存副本上补闭合并验证。
    result.open_calls = open_turn ? result.conversation.open_calls() : std::vector<ToolCall>{};
    Conversation checked = result.conversation;
    for (const ToolCall& call : result.open_calls) {
        checked.add_tool_result(call.id, std::string(texts::kCrashed), "recover interrupted call");
    }
    if (const std::optional<std::string> invalid = checked.validate()) {
        throw RecordError(RecordError::Kind::corrupt, "inconsistent session history: " + *invalid);
    }
    return result;
}

} // namespace dagent::agent
