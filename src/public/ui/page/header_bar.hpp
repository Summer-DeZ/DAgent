#pragma once
#include "ui/style/appearance.hpp"

namespace dagent::ui {
class HeaderBar final : public tui::Widget {
public:
    std::string title, mode, model, branch;
    std::size_t used = 0, limit = 0;
    int trigger = 80;
    bool sidebar = true, outline = false;
    void set_theme(const tui::ThemeTokens& t) { theme_ = &t; invalidate(); }
    tui::Size measure(tui::Size a) const override { return {a.cols, outline ? 2 : 1}; }
    void render(tui::Surface&) override;
private:
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
};
} // namespace dagent::ui
