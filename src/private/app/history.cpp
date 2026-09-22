#include "app/history.hpp"

#include <format>
#include <stdexcept>
#include <utility>

#include "agent/history.hpp"
#include "storage/history_read.hpp"

namespace dagent::app {
namespace {

/// @brief 历史条目 → 旧实时事件（只服务旧 UI 的 Transcript 回放，R13 删除）。
void append_legacy_events(const agent::HistoryItem& item, std::vector<agent::Event>& events) {
    switch (item.kind) {
    case agent::HistoryItem::Kind::system:
        if (!item.model.empty()) events.push_back(agent::ModelChanged{item.model});
        break;
    case agent::HistoryItem::Kind::user:
        events.push_back(agent::TurnStarted{item.text});
        break;
    case agent::HistoryItem::Kind::assistant:
        if (!item.reasoning.empty()) events.push_back(agent::ReasoningDelta{item.reasoning});
        if (!item.text.empty()) events.push_back(agent::TextDelta{item.text});
        break;
    case agent::HistoryItem::Kind::tool_started:
        events.push_back(item.started);
        break;
    case agent::HistoryItem::Kind::tool:
        events.push_back(agent::ToolFinished{item.call_id, item.name, item.summary, item.result});
        break;
    case agent::HistoryItem::Kind::turn_end:
        events.push_back(agent::TurnEnded{item.status, item.error, item.steps, item.tool_calls, item.usage});
        break;
    }
}

} // namespace

std::vector<agent::Event> project_history(const storage::Options& options, std::string_view id) {
    std::vector<agent::Event> events;
    std::unique_ptr<storage::HistoryRead> read = storage::HistoryRead::open(options, id);
    std::string cursor;
    for (;;) {
        storage::HistoryRead::Page page = read->read(cursor, 100);
        for (const agent::HistoryItem& item : page.items) append_legacy_events(item, events);
        if (page.done) break;
        cursor = std::move(page.cursor);
    }
    return events;
}

std::string resolve_session_id(const storage::Options& options,
                               const std::filesystem::path& cwd,
                               std::optional<std::string_view> prefix) {
    const std::vector<storage::Summary> sessions = storage::list(options, cwd, 0);
    if (!prefix) {
        if (sessions.empty()) throw std::runtime_error("no sessions for this working directory");
        return sessions.front().meta.id;
    }

    std::vector<const storage::Summary*> matches;
    for (const storage::Summary& summary : sessions) {
        if (summary.meta.id == *prefix) return summary.meta.id;
        if (summary.meta.id.starts_with(*prefix)) matches.push_back(&summary);
    }
    if (matches.empty()) throw std::runtime_error("session not found in this working directory: " + std::string(*prefix));
    if (matches.size() == 1) return matches.front()->meta.id;

    std::string message = "ambiguous session ID prefix; use a longer prefix: ";
    for (const storage::Summary* match : matches) {
        message += "\n  " + match->meta.id;
        if (!match->title.empty()) message += "  " + match->title;
    }
    throw std::runtime_error(message);
}

} // namespace dagent::app
