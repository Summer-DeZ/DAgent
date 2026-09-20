#include "ui/status_line.hpp"
#include "ui/display.hpp"
#include "ui/strings.hpp"

#include <algorithm>
#include <format>

namespace dagent::ui {
namespace {
std::string compact_tokens(std::size_t value) {
    if (value >= 1000) return format_text(ui::text().status_tokens, value / 1000.0);
    return std::to_string(value);
}
}

void StatusLine::project(std::string path) {
    if (path_ == path) return;
    path_ = std::move(path); invalidate();
}
void StatusLine::context(const agent::ContextUpdate& update) {
    if (used_ == update.used && limit_ == update.limit) return;
    used_ = update.used; limit_ = update.limit; invalidate();
}
void StatusLine::todo(int done, int total, bool shown) {
    if (todo_done_ == done && todo_total_ == total && todo_shown_ == shown) return;
    todo_done_ = done; todo_total_ = total; todo_shown_ = shown; invalidate();
}

void StatusLine::render(tui::Surface& surface) {
    const int cols = surface.cols();
    surface.fill({0, 0, cols, surface.rows()}, U' ', theme_->background);
    if (cols <= 0) return;

    const double percent = limit_ ? 100.0 * static_cast<double>(used_) / static_cast<double>(limit_) : 0;
    const tui::Style usage_style = percent >= trigger_ ? theme_->warning : theme_->text_muted;
    std::string usage = limit_ ? std::format("{} ({:.0f}%)", compact_tokens(used_), percent)
                               : compact_tokens(used_);
    if (todo_shown_ && todo_total_ > 0)
        usage = format_text(ui::text().status_plan, todo_done_, todo_total_) + "   " + usage;
    const std::string hint(ui::text().status_hint);

    const int right_width = display_width(usage) + 3 + display_width(hint);
    surface.text(0, 0, fit_columns(path_, std::max(0, cols - right_width - 2)), theme_->text_muted);
    if (right_width + 2 > cols) return;
    int col = cols - right_width;
    col = surface.text(col, 0, usage, usage_style);
    surface.text(col + 3, 0, hint, theme_->text_muted);
}

} // namespace dagent::ui
