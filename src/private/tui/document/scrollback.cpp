#include "tui/document/scrollback.hpp"

#include <algorithm>
#include <limits>

#include "tui/document/document_detail.hpp"

namespace dagent::tui {

using document_detail::walk_line;

Size Scrollback::measure(Size available) const { return available; }

/// @brief 文档版本变化也视为脏。
bool Scrollback::dirty_tree() const noexcept {
    return Widget::dirty_tree() || rendered_rev_ != doc_.revision();
}

size_t Scrollback::max_top() const noexcept {
    const size_t h = static_cast<size_t>(view_ > 0 ? view_ : 0);
    return total_ > h ? total_ - h : 0;
}

void Scrollback::anchor_to(size_t row, int dir) {
    const size_t max = max_top();
    if (row >= max) {
        doc_.set_anchor(Anchor{}); // pinned_to_bottom = true
        return;
    }
    Location loc = doc_.location_of(row, theme_);
    if (dir < 0) {
        // 边距行映射到下方块首：回退到真实内容行
        while (row > 0 &&
               doc_.row_of(loc.block_id, loc.byte_in_block, theme_).value_or(row) > row) {
            loc = doc_.location_of(--row, theme_);
        }
    }
    doc_.set_anchor(Anchor{loc.block_id, loc.byte_in_block, false});
}

void Scrollback::scroll_lines(int lines) {
    if (lines == 0) return;
    const long long max = static_cast<long long>(max_top());
    const long long target = static_cast<long long>(top_) + lines;
    anchor_to(static_cast<size_t>(target < 0 ? 0 : (target > max ? max : target)),
              lines);
    invalidate();
}

void Scrollback::scroll_pages(int pages) {
    if (pages == 0) return;
    scroll_lines(pages * (view_ > 0 ? view_ : 1));
}

void Scrollback::scroll_home() {
    anchor_to(0, 1);
    invalidate();
}

void Scrollback::scroll_end() {
    doc_.set_anchor(Anchor{}); // pinned_to_bottom = true
    invalidate();
}

void Scrollback::render(Surface& s) {
    const int w = s.cols();
    const int h = s.rows();
    if (w <= 0 || h <= 0) return;
    doc_.begin_frame(w, theme_.epoch);

    const size_t total = doc_.total_rows();
    const size_t view = static_cast<size_t>(h);
    const size_t max = total > view ? total - view : 0;
    size_t top = max;
    const Anchor anchor = doc_.anchor();
    if (!anchor.pinned_to_bottom) {
        // 锚点换算视口顶行
        const auto r = doc_.row_of(anchor.block_id, anchor.byte_in_block, theme_);
        top = std::min(r.value_or(0), max);
        // 被夹取时锚点跟随实际顶行。
        if ((!r || top != *r) && total > 0) {
            const Location loc = doc_.location_of(top, theme_);
            doc_.set_anchor(Anchor{loc.block_id, loc.byte_in_block, false});
        }
    }

    doc_.materialize_range(top, view, theme_);

    s.fill({0, 0, w, h}, U' ', theme_.background);
    for (int y = 0; y < h; ++y) {
        const size_t row = top + static_cast<size_t>(y);
        const Line* ln = doc_.line_at(row);
        if (ln != nullptr) draw_row(s, y, row, *ln);
    }
    doc_.evict_outside(top, view);

    total_ = total;
    top_ = top;
    view_ = h;
    unseen_ = total > top + view ? total - top - view : 0;
    rendered_rev_ = doc_.revision();
}

/// @brief 绘制一行；落在选区内的字素套选区样式。
void Scrollback::draw_row(Surface& s, int y, size_t row, const Line& ln) {
    int col = 0;
    const Block* blk = nullptr;
    Location lo;
    Location hi;
    if (selection_) {
        lo = std::min(selection_->anchor, selection_->head);
        hi = std::max(selection_->anchor, selection_->head);
        const uint64_t id = doc_.location_of(row, theme_).block_id;
        if (id >= lo.block_id && id <= hi.block_id) blk = doc_.find(id);
    }
    if (blk == nullptr) {
        for (const Span& sp : ln.spans) col = s.text(col, y, sp.text, sp.style);
    } else {
        walk_line(ln, blk->source,
                  [&](int, int, size_t byte, bool, std::string_view g, const Span& sp) {
                      const Location at{blk->id, byte};
                      Style st = sp.style;
                      if (!(at < lo) && !(hi < at)) {
                          // 选区令牌：设了 fg/bg 就覆盖，其余属性取并集。
                          if (theme_.selection.fg.kind != Color::Kind::default_) {
                              st.fg = theme_.selection.fg;
                          }
                          if (theme_.selection.bg.kind != Color::Kind::default_) {
                              st.bg = theme_.selection.bg;
                          }
                          // 反色取翻转而非并集。
                          if (any(theme_.selection.attrs & Attr::reverse)) {
                              st.attrs = any(st.attrs & Attr::reverse)
                                             ? st.attrs & ~Attr::reverse
                                             : st.attrs | Attr::reverse;
                          }
                          st.attrs = st.attrs | (theme_.selection.attrs & ~Attr::reverse);
                      }
                      col = s.text(col, y, g, st);
                  });
    }
    if (col < s.cols()) {
        s.fill({col, y, s.cols() - col, 1}, U' ', theme_.background);
    }
}

std::optional<Location> Scrollback::hit(Point p) {
    if (total_ == 0 || view_ <= 0) return std::nullopt;
    int x = std::max(p.x, 0);
    if (p.y < 0) x = 0;                                 // 拖出视口上方：到行首
    if (p.y >= view_) x = std::numeric_limits<int>::max(); // 下方：到行尾
    const int y = std::clamp(p.y, 0, view_ - 1);
    size_t row = top_ + static_cast<size_t>(y);
    if (row >= total_) {
        row = total_ - 1; // 视口下方的空白：视为最后一行的行尾之后
        x = std::numeric_limits<int>::max();
    }
    return doc_.location_at(row, x, theme_);
}

void Scrollback::select(Selection sel) {
    selection_ = sel;
    invalidate();
}

void Scrollback::clear_selection() {
    if (!selection_) return;
    selection_.reset();
    invalidate();
}

std::string Scrollback::selected_text() const {
    return selection_ ? doc_.text_between(selection_->anchor, selection_->head)
                      : std::string{};
}

} // namespace dagent::tui
