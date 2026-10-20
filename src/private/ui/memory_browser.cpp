#include "ui/memory_browser.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "tui/document.hpp"
#include "ui/strings.hpp"

namespace dagent::ui {

MemoryBrowser::MemoryBrowser(Panel& panel, Actions actions)
    : panel_(panel), actions_(std::move(actions)) {}

void MemoryBrowser::show_error(const protocol::RpcError& error) {
    panel_.close();
    actions_.toast("Memory: " + error.message, tui::Notice::Severity::error);
}
void MemoryBrowser::open() {
    panel_.open("Memory", {{std::string(ui::text().panel_loading), {}, {}, false, {}}},
                "Enter view · Esc close", false);
    actions_.request("memory.list", {}, [this](const nlohmann::json& value) {
        std::vector<Panel::Row> rows;
        const auto entries = value.value("entries", nlohmann::json::array());
        for (const auto scope : {"global", "project"}) {
            bool found = false;
            for (const auto& entry : entries) {
                if (entry.value("scope", "") != scope) continue;
                found = true;
                const std::string name = entry.value("name", "");
                rows.push_back({name, entry.value("description", ""),
                    entry.value("type", "") + " " + entry.value("updated", ""), true,
                    [this, scope = std::string(scope), name] { show_entry(scope, name); }, scope});
            }
            if (!found) rows.push_back({"(no memories)", {}, {}, false, {}, scope});
        }
        panel_.open("Memory", std::move(rows), "Enter view · Esc close");
    }, [this](const protocol::RpcError& error) { show_error(error); });
}
void MemoryBrowser::show_entry(const std::string& scope, const std::string& name) {
    actions_.request("memory.view", {{"scope", scope}, {"name", name}}, [this, scope, name](const nlohmann::json& value) {
        const auto& entry = value.at("view");
        std::vector<Panel::Row> rows;
        rows.push_back({"Back to memories", {}, {}, true, [this] { open(); }});
        rows.push_back({"Delete this memory", {}, {}, true, [this, scope, name] {
            actions_.request("memory.delete", {{"scope", scope}, {"name", name}}, [this](const nlohmann::json&) {
                open();
            }, [this](const protocol::RpcError& error) { show_error(error); });
        }});
        const auto body = entry.value("type", "") + " / " + entry.value("updated", "") + "\n" +
                          entry.value("description", "") + "\n\n" + entry.value("content", "");
        const int width = std::max(1, std::min(76, actions_.columns() - 8) - 4);
        for (std::size_t offset = 0; offset < body.size();) {
            const auto edge = tui::wrap_next_row(body, offset, width);
            std::string line;
            tui::expand_row(line, body, offset, edge.end);
            rows.push_back({std::move(line), {}, {}, true, {}});
            offset = edge.next;
        }
        panel_.open("Memory · " + scope + "/" + name, std::move(rows),
                    std::string(ui::text().panel_session_footer), false);
    }, [this](const protocol::RpcError& error) { show_error(error); });
}
} // namespace dagent::ui
