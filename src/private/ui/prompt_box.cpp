#include "ui/prompt_box.hpp"
#include "ui/display.hpp"

#include <algorithm>
#include <utility>

#include "tui/document.hpp"

namespace dagent::ui {
namespace {
constexpr int k_left = 2;   ///< 竖条 + 一列间隙
constexpr int k_right = 1;  ///< 右侧留白

}

void PromptBox::set_footer(std::string value) {
    if (footer_ == value) return;
    footer_ = std::move(value); invalidate();
}
void PromptBox::set_footer_tone(bool error, bool accent) {
    if (footer_error_ == error && footer_accent_ == accent) return;
    footer_error_ = error; footer_accent_ = accent; invalidate();
}
void PromptBox::set_active(bool value) {
    if (active_ == value) return;
    active_ = value; invalidate();
}
void PromptBox::set_theme(const tui::ThemeTokens& theme) {
    theme_ = &theme; tui::InputBox::set_theme(theme); invalidate();
}

int PromptBox::inner_width(int cols) const noexcept {
    return std::max(1, cols - k_left - k_right);
}
int PromptBox::text_rows(int height) const noexcept { return std::max(1, height - chrome()); }

std::vector<PromptBox::Row> PromptBox::wrap(std::string_view value, int width) {
    std::vector<Row> rows;
    std::size_t line = 0, from = 0;
    while (true) {
        const std::size_t stop = std::min(value.find('\n', from), value.size());
        std::size_t at = from;
        do { // 空行也要占一行，所以先产出再判断
            const tui::RowEdge edge =
                tui::wrap_next_row(value.substr(0, stop), at, width);
            rows.push_back({line, at, edge.end});
            at = edge.next > at ? edge.next : stop;
        } while (at < stop);
        if (stop == value.size()) break;
        from = stop + 1;
        ++line;
    }
    return rows;
}

std::optional<std::pair<int, int>> PromptBox::caret(const std::vector<Row>& rows,
                                                    std::string_view value) const {
    // 基类按自己的矩形裁剪光标，这里先把矩形放大再问，拿到未裁剪的逻辑位置。
    const tui::Rect saved = rect_;
    auto* self = const_cast<PromptBox*>(this);
    self->rect_ = {saved.x, saved.y, 1 << 16, 1 << 16};
    const std::optional<tui::Point> base = tui::InputBox::cursor();
    self->rect_ = saved;
    if (!base) return std::nullopt;

    const std::size_t line = static_cast<std::size_t>(base->y - 1);
    const int column = base->x - 1; // 逻辑行内的显示列
    int consumed = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].line != line) continue;
        const int row_width =
            display_width(value.substr(rows[i].begin, rows[i].end - rows[i].begin));
        const bool last = i + 1 >= rows.size() || rows[i + 1].line != line;
        if (column < consumed + row_width || last)
            return std::pair{static_cast<int>(i), column - consumed};
        consumed += row_width;
    }
    return std::nullopt;
}

tui::Size PromptBox::measure(tui::Size available) const {
    const std::string value = text();
    const int rows = value.empty()
                         ? 1
                         : static_cast<int>(wrap(value, inner_width(available.cols)).size());
    return {available.cols, std::clamp(rows, 1, k_max_rows) + chrome()};
}

std::optional<tui::Point> PromptBox::cursor() const {
    const int width = inner_width(rect().w);
    const std::string value = text();
    const std::vector<Row> rows = wrap(value, width);
    const auto at = caret(rows, value);
    if (!at) return tui::Point{k_left, 0};

    const int visible = text_rows(rect().h);
    const int top = std::clamp(at->first - visible + 1, 0,
                               std::max(0, static_cast<int>(rows.size()) - visible));
    const int row = at->first - top;
    if (row < 0 || row >= visible) return std::nullopt;
    return tui::Point{k_left + std::min(at->second, width - 1), row + this->top()};
}

void PromptBox::render(tui::Surface& surface) {
    const int w = surface.cols(), h = surface.rows();
    if (w <= 0 || h <= 0) return;
    surface.fill({0, 0, w, h}, U' ', (outline_ ? theme_->background : theme_->background_panel));

    tui::Style bar = active_ ? theme_->primary : theme_->accent; // 忙碌时换色，其余时候常亮
    bar.bg = (outline_ ? theme_->background : theme_->background_panel).bg;
    if (outline_) {
        auto border = theme_->border; border.bg = theme_->background.bg;
        draw_frame(surface, {0, 0, w, h}, border);
    } else for (int y = 0; y < h; ++y) surface.put(0, y, icons().bar, bar);

    tui::Style body = theme_->text;
    body.bg = (outline_ ? theme_->background : theme_->background_panel).bg;
    tui::Style muted = theme_->text_muted;
    muted.bg = (outline_ ? theme_->background : theme_->background_panel).bg;

    const std::string value = text();
    const int width = inner_width(w);
    const int visible = text_rows(h);
    if (value.empty()) {
        if (!ui::text().box_placeholder.empty())
            surface.text(k_left, this->top(), fit_columns(ui::text().box_placeholder, width), muted);
    } else {
        const std::vector<Row> rows = wrap(value, width);
        const auto at = caret(rows, value);
        const int focus = at ? at->first : static_cast<int>(rows.size()) - 1;
        const int top = std::clamp(focus - visible + 1, 0,
                                   std::max(0, static_cast<int>(rows.size()) - visible));
        std::string line;
        for (int i = 0; i < visible && top + i < static_cast<int>(rows.size()); ++i) {
            const Row& row = rows[static_cast<std::size_t>(top + i)];
            tui::expand_row(line, value, row.begin, row.end);
            surface.text(k_left, this->top() + i, line, body);
        }
    }
    if (!footer_.empty()) {
        tui::Style footer = footer_error_ ? theme_->error : footer_accent_ ? theme_->accent : muted;
        footer.bg = (outline_ ? theme_->background : theme_->background_panel).bg;
        surface.text(k_left, h - 1 - (outline_ ? 1 : 0), fit_columns(footer_, width), footer);
    }
}

} // namespace dagent::ui
