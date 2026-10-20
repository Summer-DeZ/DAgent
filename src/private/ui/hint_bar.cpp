#include "ui/hint_bar.hpp"
#include "ui/appearance.hpp"
#include "ui/display.hpp"
#include <algorithm>
namespace dagent::ui {
void HintBar::render(tui::Surface& s) {
    const auto background = outline ? theme_->background : theme_->background_panel;
    const auto style = [&](tui::Style value) { value.bg = background.bg; return value; };
    s.fill({0, 0, s.cols(), s.rows()}, U' ', background);
    const auto items = hints ? hints() : std::vector<KeyHint>{};
    painted_ = items;
    int width = 0;
    for (const auto& item : items) width += display_width(item.key) + 1 + display_width(item.description);
    width += std::max(0, static_cast<int>(items.size()) - 1) * display_width(icons().separator);
    s.text(0, 0, fit_columns(path, std::max(0, s.cols() - width - 2)), style(theme_->text_muted));
    int x = std::max(0, s.cols() - width);
    bool first = true;
    for (const auto& item : items) {
        if (!first) x = s.text(x, 0, icons().separator, style(theme_->text_faint));
        x = s.text(x, 0, item.key, style(theme_->text));
        x = s.text(x, 0, " " + item.description, style(theme_->text_muted));
        first = false;
    }
}
} // namespace dagent::ui
