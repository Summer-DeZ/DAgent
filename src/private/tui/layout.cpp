#include "tui/layout.hpp"

#include <algorithm>

namespace dagent::tui {

namespace {

int clampv(int v, int lo, int hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace

Container::Container(Direction dir) noexcept : dir_(dir) {}

void Container::add(Constraint c, std::unique_ptr<Widget> w) {
    items_.push_back({c, std::move(w)});
    invalidate_layout();
}

void Container::invalidate_tree() noexcept {
    invalidate();
    for (auto& it : items_) {
        it.widget->invalidate_tree();
    }
}

// 布局纪元提升的递归判定：任何子树声明了 invalidate_layout 就重算。
bool Container::needs_layout() const noexcept {
    if (layout_dirty_) {
        return true;
    }
    for (auto& it : items_) {
        if (it.widget->needs_layout()) {
            return true;
        }
    }
    return false;
}

void Container::layout(Rect area) {
    rect_ = area;
    distribute(area);
    layout_dirty_ = false;
    for (auto& it : items_) {
        it.widget->layout_dirty_ = false; // 嵌套容器的子树由其 layout() 自清
    }
}

// 区域分配。只在布局纪元触发时运行：终端尺寸变化或某 widget
// invalidate_layout()；其余帧沿用上一帧的 Rect 直接渲染。
void Container::distribute(Rect area) {
    const int n = static_cast<int>(items_.size());
    if (n == 0) {
        return;
    }
    const bool vert = dir_ == Direction::vertical;
    const int major = vert ? area.h : area.w;
    const int minor = vert ? area.w : area.h;

    std::vector<int> sz(static_cast<size_t>(n), 0);
    std::vector<int> flex_idx;
    int used = 0;
    int weight_sum = 0;

    // 1. fixed 直接占用
    for (int i = 0; i < n; ++i) {
        const Constraint& c = items_[static_cast<size_t>(i)].constraint;
        if (c.sizing == Sizing::fixed) {
            sz[static_cast<size_t>(i)] = clampv(c.value, c.min, c.max);
            used += sz[static_cast<size_t>(i)];
        } else if (c.sizing == Sizing::flex) {
            flex_idx.push_back(i);
            weight_sum += c.value > 0 ? c.value : 1;
        }
    }

    // 2. content 按声明顺序测量，夹取到 [min, max] 与剩余空间
    for (int i = 0; i < n; ++i) {
        const Constraint& c = items_[static_cast<size_t>(i)].constraint;
        if (c.sizing != Sizing::content) {
            continue;
        }
        const int avail = major - used > 0 ? major - used : 0;
        const Size m = items_[static_cast<size_t>(i)].widget->measure(
            vert ? Size{minor, avail} : Size{avail, minor});
        int v = vert ? m.rows : m.cols;
        v = clampv(v, c.min, c.max);
        if (v > major - used) {
            v = major - used > 0 ? major - used : 0;
        }
        sz[static_cast<size_t>(i)] = v;
        used += v;
    }

    // 3. 剩余空间按权重分给 flex
    const int rem = major - used;
    if (rem > 0 && weight_sum > 0) {
        int allocated = 0;
        for (int i : flex_idx) {
            const Constraint& c = items_[static_cast<size_t>(i)].constraint;
            const long long w = c.value > 0 ? c.value : 1;
            int v = static_cast<int>(rem * w / weight_sum); // floor
            v = clampv(v, c.min, c.max);
            sz[static_cast<size_t>(i)] = v;
            allocated += v;
        }
        // floor 损失的余量按声明顺序补 1；被 max 截住就留给能长的
        int leftover = rem - allocated;
        while (leftover > 0) {
            bool progressed = false;
            for (int i : flex_idx) {
                if (leftover == 0) break;
                const Constraint& c = items_[static_cast<size_t>(i)].constraint;
                if (sz[static_cast<size_t>(i)] < c.max) {
                    ++sz[static_cast<size_t>(i)];
                    --leftover;
                    progressed = true;
                }
            }
            if (!progressed) break;
        }
    } else if (rem < 0) {
        // 4. 超订阅：按声明顺序逆序压缩，直到 min
        int deficit = -rem;
        for (int i = n - 1; i >= 0 && deficit > 0; --i) {
            const Constraint& c = items_[static_cast<size_t>(i)].constraint;
            const int room = sz[static_cast<size_t>(i)] - c.min;
            if (room <= 0) {
                continue;
            }
            const int take = room < deficit ? room : deficit;
            sz[static_cast<size_t>(i)] -= take;
            deficit -= take;
        }
    }

    // 赋区域：Rect 变化的子项失效（旧位置内容作废，必须重画）；
    // 未变化的子项沿用渲染跳过路径。嵌套容器经虚 layout() 递归。
    int off = 0;
    for (int i = 0; i < n; ++i) {
        const int s = sz[static_cast<size_t>(i)];
        const Rect r = vert ? Rect{area.x, area.y + off, minor, s}
                            : Rect{area.x + off, area.y, s, minor};
        Widget& w = *items_[static_cast<size_t>(i)].widget;
        if (!(w.rect_ == r)) {
            w.rect_ = r;
            w.invalidate();
        }
        w.layout(r);
        off += s;
    }
}

// 容器自身的自然尺寸（作为父容器的 content 子项被测量时使用）。
Size Container::measure(Size available) const {
    int major = 0;
    int minor = 0;
    const bool vert = dir_ == Direction::vertical;
    for (auto& it : items_) {
        const Constraint& c = it.constraint;
        int m_major = 0;
        int m_minor = 0;
        switch (c.sizing) {
        case Sizing::fixed:
            m_major = clampv(c.value, c.min, c.max);
            break;
        case Sizing::content: {
            const Size cs = it.widget->measure(available);
            m_major = vert ? cs.rows : cs.cols;
            m_minor = vert ? cs.cols : cs.rows;
            m_major = clampv(m_major, c.min, c.max);
            break;
        }
        case Sizing::flex:
            m_major = c.min; // flex 的自然尺寸是下限
            break;
        }
        major += m_major;
        minor = std::max(minor, m_minor);
    }
    return vert ? Size{minor, major} : Size{major, minor};
}

// 只重画失效子项：未失效且 Rect 未变的子项在 front 里内容仍正确，
// back 起点又是 front 的拷贝 —— "只有转圈符号在动"时每帧只碰一行。
void Container::render(Surface& s) {
    for (auto& it : items_) {
        Widget& w = *it.widget;
        if (!w.dirty_ || w.rect_.empty()) {
            continue;
        }
        Surface v = s.view(w.rect_); // 结构性裁剪：子项画不进别人的区域
        w.render(v);
        w.dirty_ = false;
    }
    dirty_ = false;
}

bool Container::on_event(const Event& e) {
    if (focus_ >= 0 && focus_ < static_cast<int>(items_.size())) {
        return items_[static_cast<size_t>(focus_)].widget->on_event(e);
    }
    return false;
}

std::optional<Point> Container::cursor() const {
    if (focus_ < 0 || focus_ >= static_cast<int>(items_.size())) {
        return std::nullopt;
    }
    const Widget& w = *items_[static_cast<size_t>(focus_)].widget;
    const std::optional<Point> c = w.cursor();
    if (!c) {
        return std::nullopt;
    }
    const Rect r = w.rect_;
    return Point{r.x + c->x, r.y + c->y}; // 子坐标 → 树坐标
}

bool Container::focus_next() noexcept {
    return move_focus(1);
}

bool Container::focus_prev() noexcept {
    return move_focus(-1);
}
bool Container::move_focus(int step) noexcept {
    const int n = static_cast<int>(items_.size());
    if (n == 0) {
        return false;
    }
    for (int k = 1; k <= n; ++k) {
        const int i = ((focus_ + step * k) % n + n) % n;
        if (items_[static_cast<size_t>(i)].widget->focusable()) {
            if (focus_ >= 0 && focus_ < n) {
                items_[static_cast<size_t>(focus_)].widget->invalidate();
            }
            focus_ = i;
            items_[static_cast<size_t>(i)].widget->invalidate();
            return true;
        }
    }
    return false;
}

} // namespace dagent::tui
