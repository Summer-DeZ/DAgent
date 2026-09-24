#include "app/history.hpp"

#include <format>
#include <stdexcept>
#include <utility>

namespace dagent::app {

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
