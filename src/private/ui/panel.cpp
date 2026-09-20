#include "ui/panel.hpp"
#include "ui/strings.hpp"
#include "ui/display.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace dagent::ui {

class Panel::View final : public tui::Widget {
public:
    Panel* owner = nullptr;
    std::string title, hint;
    const tui::ThemeTokens* theme = &tui::dark_theme();

    tui::Size measure(tui::Size available) const override {
        const int rows = static_cast<int>(owner->shown_.size()) + (owner->filter_ ? 4 : 3);
        return {std::max(1, std::min(76, available.cols - 8)),
                std::max(3, std::min(rows, available.rows * 3 / 5))};
    }
    void render(tui::Surface& surface) override {
        const int w = surface.cols(), h = surface.rows();
        surface.fill({0, 0, w, h}, U' ', theme->background_panel);
        if (w < 4 || h < 3) return;
        auto panel = [&](tui::Style style) { style.bg = theme->background_panel.bg; return style; };
        const tui::Style border = panel(theme->border_active);
        surface.fill({1, 0, w - 2, 1}, U'─', border);
        surface.fill({1, h - 1, w - 2, 1}, U'─', border);
        surface.fill({0, 1, 1, h - 2}, U'│', border);
        surface.fill({w - 1, 1, 1, h - 2}, U'│', border);
        surface.put(0, 0, "╭", border); surface.put(w - 1, 0, "╮", border);
        surface.put(0, h - 1, "╰", border); surface.put(w - 1, h - 1, "╯", border);
        surface.text(2, 0, fit_columns(" " + title + " ", w - 4), panel(theme->text));
        const std::string footer = fit_columns(" " + hint + " ", w - 4);
        surface.text((w - display_width(footer)) / 2, h - 1, footer, panel(theme->text_muted));
        int y = 1;
        if (owner->filter_) {
            surface.text(2, y++, fit_columns(std::string(ui::text().panel_search) + owner->query_, w - 4), panel(theme->text_muted));
            ++y;
        }
        const int visible = std::max(0, h - y - 1);
        int first = 0;
        if (owner->selected_ >= visible) first = owner->selected_ - visible + 1;
        for (int pos = first; pos < static_cast<int>(owner->shown_.size()) && y < h - 1; ++pos, ++y) {
            const Row& row = owner->rows_[static_cast<std::size_t>(owner->shown_[pos])];
            const bool selected = pos == owner->selected_;
            if (selected) surface.fill({1, y, w - 2, 1}, U' ', theme->background_element);
            auto row_style = [&](tui::Style style) {
                style.bg = selected ? theme->background_element.bg : theme->background_panel.bg;
                return style;
            };
            surface.text(1, y, selected ? "▌" : " ", row_style(selected ? theme->primary : theme->text_muted));
            const tui::Style style = row_style(row.enabled ? theme->text : theme->text_muted);
            const std::string right = fit_columns(row.right, std::max(0, (w - 4) / 3));
            const int right_col = w - 2 - display_width(right);
            const int label_end = !row.middle.empty() ? w / 2 - 2
                                : !right.empty() ? right_col - 2 : w - 2;
            surface.text(2, y, fit_columns(row.label, label_end - 2), style);
            if (!row.middle.empty()) surface.text(w / 2, y,
                fit_columns(row.middle, right_col - w / 2 - 2), row_style(theme->text_muted));
            if (!right.empty()) surface.text(right_col, y, right, row_style(theme->text_muted));
        }
    }
};

Panel::Panel(tui::Runtime& rt) : rt_(rt) {}
Panel::~Panel() { on_close_ = {}; close(); }
void Panel::open(std::string title, std::vector<Row> rows, std::string hint,
                 bool filter, std::function<void(int)> on_highlight,
                 std::function<void(bool)> on_close, int initial_row,
                 std::function<void()> on_add) {
    close(); rows_ = std::move(rows); filter_ = filter; on_highlight_ = std::move(on_highlight);
    on_close_ = std::move(on_close); on_add_ = std::move(on_add);
    query_.clear(); selected_ = 0;
    auto view = std::make_unique<View>(); view_ = view.get(); view_->owner = this;
    view_->title = std::move(title); view_->hint = std::move(hint); view_->theme = &theme_;
    refilter();
    if (initial_row >= 0 && initial_row < static_cast<int>(shown_.size()) && rows_[initial_row].enabled)
        selected_ = initial_row;
    overlay_ = rt_.open_overlay(std::move(view), tui::Placement::center, {}, this);
}
void Panel::close(bool committed) {
    if (!overlay_) return;
    auto callback = std::move(on_close_);
    rt_.close_overlay(overlay_); overlay_ = 0; view_ = nullptr;
    rows_.clear(); shown_.clear(); query_.clear(); on_highlight_ = {}; on_close_ = {}; on_add_ = {};
    if (callback) callback(committed);
}
void Panel::set_theme(const tui::ThemeTokens& theme) {
    theme_ = theme; if (view_) { view_->theme = &theme_; view_->invalidate(); }
}
void Panel::refilter() {
    shown_.clear();
    std::string needle = query_;
    std::ranges::transform(needle, needle.begin(), [](unsigned char c) { return std::tolower(c); });
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        std::string label = rows_[static_cast<std::size_t>(i)].label;
        std::ranges::transform(label, label.begin(), [](unsigned char c) { return std::tolower(c); });
        if (needle.empty() || label.find(needle) != std::string::npos) shown_.push_back(i);
    }
    selected_ = 0;
    while (selected_ < static_cast<int>(shown_.size()) &&
           !rows_[static_cast<std::size_t>(shown_[selected_])].enabled) ++selected_;
    if (selected_ >= static_cast<int>(shown_.size())) selected_ = 0;
    if (view_) view_->invalidate_layout();
}
void Panel::move(int delta) {
    if (shown_.empty()) return;
    int pos = selected_;
    for (int n = 0; n < static_cast<int>(shown_.size()); ++n) {
        pos = (pos + delta + static_cast<int>(shown_.size())) % static_cast<int>(shown_.size());
        if (rows_[static_cast<std::size_t>(shown_[pos])].enabled) break;
    }
    if (pos == selected_) return;
    selected_ = pos; view_->invalidate();
    if (on_highlight_) on_highlight_(shown_[selected_]);
}
bool Panel::on_event(const tui::Event& event) {
    if (!view_) return false;
    if (event.key == tui::Key::escape || (tui::any(event.mods & tui::Mods::ctrl) && event.text == "c")) {
        close(); return true;
    }
    if (event.key == tui::Key::up) { move(-1); return true; }
    if (event.key == tui::Key::down) { move(1); return true; }
    if (event.kind == tui::Event::Kind::text && event.text == "a" &&
        event.mods == tui::Mods::none && on_add_) {
        auto action = std::move(on_add_); close(true); action(); return true;
    }
    if (event.key == tui::Key::enter) {
        if (!shown_.empty() && rows_[shown_[selected_]].enabled) {
            auto action = rows_[static_cast<std::size_t>(shown_[selected_])].activate;
            close(true); if (action) action();
        }
        return true;
    }
    if (!filter_) return true;
    if (event.key == tui::Key::backspace) {
        if (!query_.empty()) {
            do { query_.pop_back(); } while (!query_.empty() &&
                (static_cast<unsigned char>(query_.back()) & 0xc0) == 0x80);
            refilter();
        }
        return true;
    }
    if (event.kind == tui::Event::Kind::text && !event.text.empty() && event.mods == tui::Mods::none) {
        query_ += event.text; refilter(); return true;
    }
    return true;
}

} // namespace dagent::ui
