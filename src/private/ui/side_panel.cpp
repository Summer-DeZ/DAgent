#include "ui/side_panel.hpp"
#include "ui/display.hpp"
#include "ui/strings.hpp"

#include <algorithm>
#include <format>

namespace dagent::ui {
namespace {
/// 千分位，和上下文用量一起显示时更容易读。
std::string grouped(std::size_t value) {
    std::string digits = std::to_string(value), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}
}

void SidePanel::set_title(std::string value) {
    if (title_ == value) return;
    title_ = std::move(value); invalidate();
}
void SidePanel::set_context(std::size_t used, std::size_t limit) {
    if (used_ == used && limit_ == limit) return;
    used_ = used; limit_ = limit; invalidate();
}
void SidePanel::set_items(std::vector<tools::TodoItem> items) {
    if (items_ == items) return;
    items_ = std::move(items); invalidate();
}
void SidePanel::set_mcp(std::string value) {
    if (mcp_ == value) return;
    mcp_ = std::move(value); invalidate();
}
void SidePanel::set_project(std::string path, std::string branch) {
    std::string value = branch.empty() ? path : format_text(ui::text().panel_project, path, branch);
    if (project_ == value) return;
    project_ = std::move(value); invalidate();
}
void SidePanel::set_version(std::string value) {
    value = format_text(ui::text().panel_version, value);
    if (version_ == value) return;
    version_ = std::move(value); invalidate();
}
void SidePanel::set_collapsed(bool value) {
    if (collapsed_ == value) return;
    collapsed_ = value; invalidate_layout();
}

std::pair<int, int> SidePanel::progress() const noexcept {
    const int done = static_cast<int>(std::ranges::count_if(items_, [](const auto& item) {
        return item.state == tools::TodoItem::State::done;
    }));
    return {done, static_cast<int>(items_.size())};
}

tui::Size SidePanel::measure(tui::Size available) const {
    if (collapsed_ || available.cols < 80) return {};
    return {available.cols >= 100 ? 30 : 26, available.rows};
}

void SidePanel::render(tui::Surface& surface) {
    const int w = surface.cols(), h = surface.rows();
    if (w < 6 || h < 3) return;
    surface.fill({0, 0, w, h}, U' ', theme_->background_panel);
    const auto panel_style = [&](tui::Style style) {
        style.bg = theme_->background_panel.bg;
        return style;
    };
    surface.fill({0, 0, 1, h}, U'│', panel_style(theme_->border));

    const int x = 2, width = w - x - 1;
    tui::Style heading = theme_->text;
    heading.attrs = heading.attrs | tui::Attr::bold;

    int y = 0;
    const auto line = [&](std::string_view value, const tui::Style& style) {
        if (y >= h - 2) return;
        surface.text(x, y++, fit_columns(value, width), panel_style(style));
    };
    const auto gap = [&] { if (y < h - 2) ++y; };

    if (!title_.empty()) { line(title_, heading); gap(); }

    line(ui::text().panel_context, heading);
    if (limit_ > 0) {
        line(format_text(ui::text().panel_tokens, grouped(used_)), theme_->text_muted);
        const double percent = 100.0 * static_cast<double>(used_) / static_cast<double>(limit_);
        line(format_text(ui::text().panel_used, static_cast<int>(percent)), theme_->text_muted);
    } else {
        line(format_text(ui::text().panel_tokens, grouped(used_)), theme_->text_muted);
    }
    if (!mcp_.empty()) { gap(); line(mcp_, theme_->text_muted); }

    if (!items_.empty()) {
        gap();
        const auto [done, total] = progress();
        line(format_text(ui::text().panel_plan_progress, ui::text().panel_plan_heading, done, total),
             heading);
        const int room = std::max(0, h - 2 - y);
        int first = 0;
        const auto doing = std::ranges::find_if(items_, [](const auto& item) {
            return item.state == tools::TodoItem::State::doing;
        });
        if (doing != items_.end() && static_cast<int>(items_.size()) > room)
            first = std::max(0, static_cast<int>(doing - items_.begin()) - 2);
        if (first > 0) line(format_text(ui::text().todo_more, first), theme_->text_muted);
        for (int i = first; i < static_cast<int>(items_.size()) && y < h - 2; ++i) {
            const auto& item = items_[static_cast<std::size_t>(i)];
            std::string_view symbol = "○";
            tui::Style symbol_style = theme_->text_muted;
            tui::Style text_style = theme_->text;
            if (item.state == tools::TodoItem::State::doing) {
                symbol = "●"; symbol_style = theme_->accent;
                text_style.attrs = text_style.attrs | tui::Attr::bold;
            } else if (item.state == tools::TodoItem::State::done) {
                symbol = "✓"; symbol_style = theme_->success; text_style = theme_->text_muted;
            } else if (item.state == tools::TodoItem::State::dropped) {
                symbol = "✗"; text_style = theme_->text_muted;
            }
            surface.text(x, y, symbol, panel_style(symbol_style));
            surface.text(x + 2, y, fit_columns(item.text, width - 2), panel_style(text_style));
            ++y;
        }
    }

    if (!project_.empty())
        surface.text(x, h - 2, fit_columns(project_, width), panel_style(theme_->text_muted));
    if (!version_.empty())
        surface.text(x, h - 1, fit_columns(version_, width), panel_style(theme_->text_muted));
}

} // namespace dagent::ui
