#pragma once

#include <algorithm>
#include "tui/runtime/input.hpp"
#include "ui/appearance.hpp"
#include "ui/projection.hpp"

namespace dagent::ui {
class WelcomeView final : public tui::Widget, public tui::EventHandler {
public:
    struct Session { std::string id, title, age; };
    bool visible = false;
    bool sessions_in_navigator = false;
    bool has_recent() const { return !sessions_in_navigator && !sessions.empty(); }
    int selected = -1, top = 0;
    int content_rows = 0;
    bool reveal_selection = false;
    void scroll(int rows) { top = std::max(0, top + rows); reveal_selection = false; invalidate(); }
    bool on_event(const tui::Event&) override;
    std::string path, branch, model, mode, notice;
    std::vector<McpStatus> mcp;
    std::vector<Session> sessions;
    std::size_t memories = 0;
    Appearance appearance;
    void set_theme(const tui::ThemeTokens& t) { theme_ = &t; invalidate(); }
    tui::Size measure(tui::Size available) const override { return available; }
    void render(tui::Surface&) override;
private:
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
};
} // namespace dagent::ui
