#include "ui/toast.hpp"
#include "ui/display.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <utility>

#include "tui/grapheme.hpp"

namespace dagent::ui {
namespace {
using namespace std::chrono_literals;
int text_width(std::string_view value) {
    int result = 0;
    tui::unicode::Grapheme g;
    while (tui::unicode::next_grapheme(value, g)) result += g.width;
    return result;
}
}

class ToastStack::View final : public tui::Widget {
public:
    struct Entry { std::uint64_t id; tui::Notice::Severity severity; std::string text; };
    std::deque<Entry> entries;
    const tui::ThemeTokens* theme = &tui::dark_theme();

    tui::Size measure(tui::Size available) const override {
        int width = 0;
        for (const auto& entry : entries) width = std::max(width, text_width(entry.text) + 6);
        return {std::min(48, std::min(available.cols, width)),
                std::min(available.rows, static_cast<int>(entries.size()) * 4 - 1)};
    }
    void render(tui::Surface& surface) override {
        surface.fill({0, 0, surface.cols(), surface.rows()}, U' ', theme->background);
        int y = 0;
        for (const auto& entry : entries) {
            if (y + 2 >= surface.rows()) break;
            const int w = surface.cols();
            auto panel = [&](tui::Style style) { style.bg = theme->background_panel.bg; return style; };
            const tui::Style border = panel(theme->border);
            surface.fill({1, y, w - 2, 1}, U'─', border);
            surface.fill({1, y + 2, w - 2, 1}, U'─', border);
            surface.put(0, y, "╭", border); surface.put(w - 1, y, "╮", border);
            surface.put(0, y + 1, "│", border); surface.put(w - 1, y + 1, "│", border);
            surface.put(0, y + 2, "╰", border); surface.put(w - 1, y + 2, "╯", border);
            std::string_view prefix = "· ";
            tui::Style style = theme->text_muted;
            if (entry.severity == tui::Notice::Severity::warn) { prefix = "⚠ "; style = theme->warning; }
            if (entry.severity == tui::Notice::Severity::error) { prefix = "✗ "; style = theme->error; }
            style = panel(style);
            int x = surface.text(2, y + 1, prefix, style);
            surface.text(x, y + 1, fit_columns(entry.text, w - x - 2), style);
            y += 4;
        }
    }
};

ToastStack::ToastStack(tui::Runtime& rt) : rt_(rt) {}
ToastStack::~ToastStack() { if (overlay_) rt_.close_overlay(overlay_); }

void ToastStack::show(tui::Notice::Severity severity, std::string text) {
    if (text.empty()) return;
    if (!view_) {
        auto view = std::make_unique<View>();
        view_ = view.get(); view_->theme = &theme_;
        overlay_ = rt_.open_overlay(std::move(view), tui::Placement::top_right);
    }
    const std::uint64_t id = next_id_++;
    view_->entries.push_front({id, severity, std::move(text)});
    if (view_->entries.size() > 3) view_->entries.pop_back();
    view_->invalidate_layout();
    rt_.after(5s, [this, id] { expire(id); });
}
void ToastStack::expire(std::uint64_t id) {
    if (!view_) return;
    std::erase_if(view_->entries, [id](const View::Entry& entry) { return entry.id == id; });
    if (view_->entries.empty()) {
        rt_.close_overlay(overlay_); overlay_ = 0; view_ = nullptr;
    } else view_->invalidate_layout();
}
void ToastStack::set_theme(const tui::ThemeTokens& theme) {
    theme_ = theme;
    if (view_) { view_->theme = &theme_; view_->invalidate(); }
}

} // namespace dagent::ui
