#include "agent/history.hpp"

#include <format>
#include <utility>

namespace dagent::agent {
namespace {

[[noreturn]] void corrupt(std::string_view what) {
    throw RecordError(RecordError::Kind::corrupt, std::format("corrupt session record: {}", what));
}

} // namespace

std::vector<HistoryItem> HistoryProjector::project(const StoredRecord& record,
                                                   const record_codec::DecodedRecord& decoded) const {
    std::vector<HistoryItem> items;
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, record_codec::SystemRecord>) {
                HistoryItem item;
                item.kind = HistoryItem::Kind::system;
                item.seq = record.seq;
                item.model = value.model;
                items.push_back(std::move(item));
            } else if constexpr (std::is_same_v<T, record_codec::UserRecord>) {
                HistoryItem item;
                item.kind = HistoryItem::Kind::user;
                item.seq = record.seq;
                item.text = value.text;
                items.push_back(std::move(item));
            } else if constexpr (std::is_same_v<T, record_codec::AssistantRecord>) {
                HistoryItem item;
                item.kind = HistoryItem::Kind::assistant;
                item.seq = record.seq;
                item.text = value.message.content;
                item.reasoning = value.message.reasoning_content;
                item.finish = value.finish;
                items.push_back(std::move(item));
            } else if constexpr (std::is_same_v<T, record_codec::ToolStartedRecord>) {
                HistoryItem item;
                item.kind = HistoryItem::Kind::tool_started;
                item.seq = record.seq;
                item.started = value.event;
                items.push_back(std::move(item));
            } else if constexpr (std::is_same_v<T, record_codec::ToolRecord>) {
                HistoryItem item;
                item.kind = HistoryItem::Kind::tool;
                item.seq = record.seq;
                item.call_id = value.call.id;
                item.name = value.call.name;
                item.summary = value.summary;
                item.result = value.result;
                items.push_back(std::move(item));
            } else if constexpr (std::is_same_v<T, record_codec::PermissionRecord> ||
                                 std::is_same_v<T, record_codec::ParentReviewRecord>) {
                const auto& decision = [&]() -> const record_codec::PermissionRecord& {
                    if constexpr (std::is_same_v<T, record_codec::ParentReviewRecord>) return value.review;
                    else return value;
                }();
                HistoryItem item;
                item.kind = std::is_same_v<T, record_codec::ParentReviewRecord>
                                ? HistoryItem::Kind::parent_review : HistoryItem::Kind::permission;
                item.seq = record.seq;
                item.call_id = decision.call_id;
                item.text = decision.summary;
                item.model = decision.model;
                item.audit = decision.audit;
                if constexpr (std::is_same_v<T, record_codec::ParentReviewRecord>)
                    item.usage = decision.usage;
                items.push_back(std::move(item));
            } else if constexpr (std::is_same_v<T, record_codec::TurnEndRecord>) {
                HistoryItem item;
                item.kind = HistoryItem::Kind::turn_end;
                item.seq = record.seq;
                item.status = value.status;
                item.error = value.error;
                item.steps = value.steps;
                item.tool_calls = value.tool_calls;
                item.usage = value.usage;
                items.push_back(std::move(item));
            }
            // permission_revoked / prune / compaction：不产生显示条目。
        },
        decoded);
    return items;
}

void HistoryCursor::observe(const record_codec::DecodedRecord& record) {
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, record_codec::SystemRecord>) {
                if (first_entry_) saw_system_ = true;
            } else if constexpr (std::is_same_v<T, record_codec::UserRecord>) {
                if (!saw_system_) corrupt("first core record is not system");
                if (value.n != next_ordinal_)
                    corrupt(std::format("expected message ordinal {}, got {}", next_ordinal_, value.n));
                if (!open_calls_.empty()) corrupt("user message while tool calls are still open");
                if (!first_entry_) safe_cut_ordinals_.insert(value.n);
                first_entry_ = false;
                ++next_ordinal_;
            } else if constexpr (std::is_same_v<T, record_codec::AssistantRecord>) {
                if (!saw_system_) corrupt("first core record is not system");
                if (value.n != next_ordinal_)
                    corrupt(std::format("expected message ordinal {}, got {}", next_ordinal_, value.n));
                if (!open_calls_.empty()) corrupt("assistant message while tool calls are still open");
                if (value.message.content.empty() && value.message.tool_calls.empty())
                    corrupt(std::format("assistant message {} has neither content nor tool calls", value.n));
                if (!first_entry_) safe_cut_ordinals_.insert(value.n);
                first_entry_ = false;
                for (const ToolCall& call : value.message.tool_calls) open_calls_.push_back(call.id);
                ++next_ordinal_;
            } else if constexpr (std::is_same_v<T, record_codec::ToolRecord>) {
                if (!saw_system_) corrupt("first core record is not system");
                if (value.n != next_ordinal_)
                    corrupt(std::format("expected message ordinal {}, got {}", next_ordinal_, value.n));
                if (open_calls_.empty()) corrupt("tool message has no matching assistant call");
                if (open_calls_.front() != value.call.id)
                    corrupt("tool message ID does not match the call");
                open_calls_.pop_front();
                tool_ordinals_.insert(value.n);
                first_entry_ = false;
                ++next_ordinal_;
            } else if constexpr (std::is_same_v<T, record_codec::ToolStartedRecord>) {
                // 审计记录，不参与配对。
            } else if constexpr (std::is_same_v<T, record_codec::PermissionRecord> ||
                                 std::is_same_v<T, record_codec::ParentReviewRecord>) {
                // 审计记录，不恢复授权。
            } else if constexpr (std::is_same_v<T, record_codec::PermissionRevokedRecord>) {
                // 审计记录。
            } else if constexpr (std::is_same_v<T, record_codec::PruneRecord>) {
                for (const std::int64_t n : value.ordinals) {
                    if (!tool_ordinals_.contains(n)) corrupt("pruned ordinal has no matching tool message");
                }
            } else if constexpr (std::is_same_v<T, record_codec::CompactionRecord>) {
                if (value.keep_from < 0 || !safe_cut_ordinals_.contains(value.keep_from))
                    corrupt("summary cut does not exist or is unsafe");
            } else if constexpr (std::is_same_v<T, record_codec::TurnEndRecord>) {
                if (!open_calls_.empty()) corrupt("turn_end while tool calls are still open");
            }
        },
        record);
}

} // namespace dagent::agent
