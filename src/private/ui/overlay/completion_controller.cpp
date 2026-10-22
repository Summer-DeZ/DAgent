#include "ui/overlay/completion_controller.hpp"

#include <cctype>
#include <chrono>
#include <utility>

#include "ui/style/strings.hpp"

namespace dagent::ui {
namespace {
using namespace std::chrono_literals;

std::vector<int> subsequence_hits(std::string_view query, std::string_view value) {
    std::vector<int> hits;
    std::size_t from = 0;
    for (const char raw : query) {
        const char target = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
        std::size_t index = from;
        while (index < value.size() &&
               static_cast<char>(std::tolower(static_cast<unsigned char>(value[index]))) != target)
            ++index;
        if (index == value.size()) break;
        hits.push_back(static_cast<int>(index++));
        from = index;
    }
    return hits;
}

} // namespace

CompletionController::CompletionController(tui::Runtime& rt, PromptBox& input, Actions actions)
    : rt_(rt), input_(input), actions_(std::move(actions)),
      completion_(rt, input, [this] { kind_.clear(); }) {}

CompletionController::~CompletionController() {
    rt_.cancel(file_debounce_);
}

void CompletionController::update() {
    const std::string value = input_.text();
    if (value.starts_with('/') && value.find('\n') == std::string::npos) {
        if (kind_ != "command") {
            kind_ = "command";
            completion_.open(std::string(ui::text().panel_commands), [this](std::string_view query, auto done) {
                std::vector<Completion::Item> items;
                for (const auto& command : actions_.commands()) {
                    if (command.alias.empty() || !command.alias.starts_with(query)) continue;
                    items.push_back({command.alias, command.alias, command.title, {}});
                }
                done(std::move(items));
            }, std::string(ui::text().panel_complete_footer),
            [this](const Completion::Item& item, bool tab) {
                input_.set_text(tab ? item.value : "");
                if (!tab) actions_.submit(item.value);
            });
        }
        completion_.refresh(value); return;
    }
    const std::size_t token = value.find_last_of(" \t\r\n");
    const std::size_t begin = token == std::string::npos ? 0 : token + 1;
    if (begin < value.size() && value[begin] == '$') {
        if (kind_ != "skill") {
            kind_ = "skill";
            completion_.open(std::string(ui::text().panel_skills), [this](std::string_view query, auto done) {
                actions_.request("skills.list", {}, [query = std::string(query), done = std::move(done)](const nlohmann::json& value) mutable {
                    std::vector<Completion::Item> items;
                    for (const auto& skill : value.value("skills", nlohmann::json::array())) {
                        const std::string name = skill.value("name", "");
                        const auto hits = subsequence_hits(query, name);
                        if (query.empty() || hits.size() == query.size())
                            items.push_back({"$" + name, name, skill.value("description", ""), hits});
                    }
                    done(std::move(items));
                });
            }, std::string(ui::text().panel_skill_footer),
            [this](const Completion::Item& item, bool) {
                std::string text = input_.text();
                const std::size_t split = text.find_last_of(" \t\r\n");
                text.erase(split == std::string::npos ? 0 : split + 1);
                text += item.value + " "; input_.set_text({}); input_.insert(text);
            });
        }
        completion_.refresh(std::string_view(value).substr(begin + 1)); return;
    }
    if (begin < value.size() && value[begin] == '@') {
        if (kind_ != "file") {
            kind_ = "file";
            completion_.open(std::string(ui::text().panel_files), [this](std::string_view query, auto done) {
                rt_.cancel(file_debounce_);
                file_debounce_ = rt_.after(80ms, [this, query = std::string(query), done = std::move(done)]() mutable {
                    nlohmann::json params{{"session_id", actions_.session_id()}, {"text", query}, {"limit", 8}};
                    actions_.request("workspace.complete", std::move(params), [done = std::move(done), query](const nlohmann::json& value) mutable {
                        std::vector<Completion::Item> items;
                        for (const auto& candidate : value.value("candidates", nlohmann::json::array())) {
                            const std::string path = candidate.value("path", "");
                            items.push_back({path, path, {}, subsequence_hits(query, path)});
                        }
                        done(std::move(items));
                    });
                });
            }, std::string(ui::text().panel_file_footer),
            [this](const Completion::Item& item, bool) {
                std::string text = input_.text();
                const std::size_t split = text.find_last_of(" \t\r\n");
                text.erase(split == std::string::npos ? 0 : split + 1);
                text += item.value + " "; input_.set_text(std::move(text));
            });
        }
        completion_.refresh(std::string_view(value).substr(begin + 1)); return;
    }
    completion_.close();
}

} // namespace dagent::ui
