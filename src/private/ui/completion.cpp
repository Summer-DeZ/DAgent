#include "ui/completion.hpp"
#include "ui/display.hpp"

#include <algorithm>
#include <utility>

#include "tui/grapheme.hpp"

namespace dagent::ui {

class Completion::View final : public tui::Widget {
public:
    Completion* owner = nullptr;
    std::string title, hint;
    const tui::ThemeTokens* theme = &tui::dark_theme();

    tui::Size measure(tui::Size available) const override {
        const int width = owner->prompt_.screen_rect().w;
        return {std::max(1, std::min(available.cols, width > 0 ? width : 40)),
                std::min(10, static_cast<int>(owner->items_.size()) + 2)};
    }
    void render(tui::Surface& surface) override {
        const int w = surface.cols(), h = surface.rows();
        surface.fill({0, 0, w, h}, U' ', theme->background_panel);
        if (w < 4 || h < 2) return;
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
        int label_width = 0;
        bool has_detail = false;
        for (const auto& item : owner->items_) {
            label_width = std::max(label_width, display_width(item.label));
            has_detail = has_detail || !item.detail.empty();
        }
        const int inner = w - 4;
        label_width = has_detail ? std::min(label_width, std::max(1, inner / 2)) : inner;
        const int detail_col = 2 + label_width + 2;
        const int capacity = std::max(0, h - 2);
        int first = owner->selected_ >= capacity ? owner->selected_ - capacity + 1 : 0;
        for (int pos = first, y = 1; pos < static_cast<int>(owner->items_.size()) && y < h - 1;
             ++pos, ++y) {
            const auto& item = owner->items_[static_cast<std::size_t>(pos)];
            const bool selected = pos == owner->selected_;
            if (selected) surface.fill({1, y, w - 2, 1}, U' ', theme->background_element);
            auto row_style = [&](tui::Style style) {
                style.bg = selected ? theme->background_element.bg : theme->background_panel.bg;
                return style;
            };
            surface.text(1, y, selected ? "▌" : " ", row_style(selected ? theme->primary : theme->text_muted));
            int col = 2;
            const std::string label = fit_columns(item.label, label_width);
            std::string_view rest = label;
            std::size_t byte = 0;
            tui::unicode::Grapheme grapheme;
            while (tui::unicode::next_grapheme(rest, grapheme)) {
                const bool hit = std::ranges::find(item.hits, static_cast<int>(byte)) != item.hits.end();
                col = surface.text(col, y, grapheme.bytes, row_style(hit ? theme->primary : theme->text));
                byte += grapheme.bytes.size();
            }
            if (!item.detail.empty() && detail_col < w - 2)
                surface.text(detail_col, y, fit_columns(item.detail, w - 2 - detail_col),
                             row_style(theme->text_muted));
        }
    }
};

Completion::Completion(tui::Runtime& rt, PromptBox& prompt, std::function<void()> closed)
    : rt_(rt), prompt_(prompt), closed_(std::move(closed)) {}
Completion::~Completion() { close(); }
void Completion::open(std::string title, Source source, std::string hint, Accept accept) {
    close(); source_ = std::move(source); accept_ = std::move(accept); selected_ = 0;
    auto view = std::make_unique<View>(); view_ = view.get(); view_->owner = this;
    view_->title = std::move(title); view_->hint = std::move(hint); view_->theme = &theme_;
    const auto rect = prompt_.screen_rect();
    overlay_ = rt_.open_overlay(std::move(view), tui::Placement::above_point,
                                {rect.x, rect.y}, this);
}
void Completion::refresh(std::string_view query) {
    if (!view_ || !source_) return;
    const auto generation = ++generation_;
    source_(query, [this, generation](std::vector<Item> items) {
        rt_.post([this, generation, items = std::move(items)]() mutable {
            if (!view_ || generation != generation_) return;
            items_ = std::move(items); selected_ = 0;
            if (items_.empty()) close();
            else view_->invalidate_layout();
        });
    });
}
void Completion::close() {
    ++generation_;
    const bool was_visible = overlay_ != 0;
    if (overlay_) rt_.close_overlay(overlay_);
    overlay_ = 0; view_ = nullptr; source_ = {}; accept_ = {}; items_.clear(); selected_ = 0;
    if (was_visible && closed_) closed_();
}
void Completion::set_theme(const tui::ThemeTokens& theme) {
    theme_ = theme; if (view_) { view_->theme = &theme_; view_->invalidate(); }
}
void Completion::accept(bool complete_only) {
    if (items_.empty()) return;
    const Item item = items_[static_cast<std::size_t>(selected_)];
    auto callback = std::move(accept_);
    close();
    if (callback) callback(item, complete_only);
    else prompt_.set_text(item.value + (complete_only ? "" : " "));
}
bool Completion::on_event(const tui::Event& event) {
    if (!view_) return false;
    if (event.key == tui::Key::escape) { close(); return true; }
    if (event.key == tui::Key::up || event.key == tui::Key::down) {
        if (!items_.empty()) {
            const int delta = event.key == tui::Key::up ? -1 : 1;
            selected_ = (selected_ + delta + static_cast<int>(items_.size())) % static_cast<int>(items_.size());
            view_->invalidate();
        }
        return true;
    }
    if (event.key == tui::Key::tab) { accept(true); return true; }
    if (event.key == tui::Key::enter) { accept(false); return true; }
    if (tui::any(event.mods & tui::Mods::ctrl) && event.text == "c") { close(); return true; }
    return false;
}

} // namespace dagent::ui
