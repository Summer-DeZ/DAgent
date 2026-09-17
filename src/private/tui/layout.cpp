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

// 子树重画判定的递归聚合：孙控件 invalidate 时本容器返回 true，
// 否则父容器会把整个子树跳过（深层失效无人重画）。gap_ 待清时
// 同样必须经过一次 render。
bool Container::dirty_tree() const noexcept {
    if (dirty_ || !gap_.empty()) {
        return true;
    }
    for (auto& it : items_) {
        if (it.widget->dirty_tree()) {
            return true;
        }
    }
    return false;
}

void Container::layout(Rect area) {
    // 子区域是局部坐标：容器移动或变形时子项 Rect 可能不变，
    // 但屏幕位置已变，整棵子树都得重画。
    const bool changed = !(rect_ == area);
    Widget::layout(area);
    distribute(area);
    if (changed) {
        invalidate_tree();
    }
}

// 区域分配。五个步骤见类注释。子区域用本容器局部坐标（原点为容器
// 矩形左上角），render 的视图裁剪与之自然对齐；Rect 变化的失效统一
// 由 Widget::layout 检测。
void Container::distribute(Rect area) {
    const bool vert = dir_ == Direction::vertical;
    const int major = vert ? area.h : area.w;
    const int minor = vert ? area.w : area.h;

    if (items_.empty()) {
        gap_ = vert ? Rect{0, 0, minor, major} : Rect{0, 0, major, minor};
        return;
    }
    const int n = static_cast<int>(items_.size());

    std::vector<int> sz(static_cast<std::size_t>(n), 0);
    std::vector<int> flex_idx;
    int used = 0;
    int weight_sum = 0;

    // 1. fixed 直接占用
    for (int i = 0; i < n; ++i) {
        const Constraint& c = items_[static_cast<std::size_t>(i)].constraint;
        if (c.sizing == Sizing::fixed) {
            sz[static_cast<std::size_t>(i)] = clampv(c.value, c.min, c.max);
            used += sz[static_cast<std::size_t>(i)];
        } else if (c.sizing == Sizing::flex) {
            flex_idx.push_back(i);
            weight_sum += c.value > 0 ? c.value : 1;
        }
    }

    // 2. content 按声明顺序测量，夹到 [min, max]。不做剩余空间截断：
    //    超订阅统一交给第 4 步的收缩步，否则 min 会被剩余空间压穿。
    for (int i = 0; i < n; ++i) {
        const Constraint& c = items_[static_cast<std::size_t>(i)].constraint;
        if (c.sizing != Sizing::content) {
            continue;
        }
        const int avail = major - used > 0 ? major - used : 0;
        const Size m = items_[static_cast<std::size_t>(i)].widget->measure(
            vert ? Size{minor, avail} : Size{avail, minor});
        const int v = clampv(vert ? m.rows : m.cols, c.min, c.max);
        sz[static_cast<std::size_t>(i)] = v;
        used += v;
    }

    // 3. 剩余空间分给 flex
    const int rem = major - used;
    if (weight_sum > 0) {
        if (rem > 0) {
            int allocated = 0;
            for (int i : flex_idx) {
                const Constraint& c =
                    items_[static_cast<std::size_t>(i)].constraint;
                const long long w = c.value > 0 ? c.value : 1;
                int v = static_cast<int>(rem * w / weight_sum); // floor
                v = clampv(v, c.min, c.max); // min 可能把总和顶出剩余空间
                sz[static_cast<std::size_t>(i)] = v;
                allocated += v;
            }
            // floor 损失的余量按声明顺序补 1；被 max 截住就留给能长的
            int leftover = rem - allocated;
            while (leftover > 0) {
                bool progressed = false;
                for (int i : flex_idx) {
                    if (leftover == 0) break;
                    const Constraint& c =
                        items_[static_cast<std::size_t>(i)].constraint;
                    if (sz[static_cast<std::size_t>(i)] < c.max) {
                        ++sz[static_cast<std::size_t>(i)];
                        --leftover;
                        progressed = true;
                    }
                }
                if (!progressed) break;
            }
        } else {
            // 剩余空间 ≤ 0：flex 先按 min 占位，交给第 4 步统一收缩；
            // 否则此情形下 min 永不生效（flex 直接得 0）。
            for (int i : flex_idx) {
                const Constraint& c =
                    items_[static_cast<std::size_t>(i)].constraint;
                sz[static_cast<std::size_t>(i)] = clampv(0, c.min, c.max);
            }
        }
    }

    // 4. 收缩步：总需求超出 major 时按声明顺序逆序压缩，直到 min。
    //    覆盖三类情形：fixed/content 超订阅（rem < 0）、flex 的 min
    //    夹取把总和顶出剩余空间、剩余空间 ≤ 0 时 flex 按 min 占位。
    int total = 0;
    for (int s : sz) total += s;
    if (total > major) {
        int deficit = total - major;
        for (int i = n - 1; i >= 0 && deficit > 0; --i) {
            const Constraint& c = items_[static_cast<std::size_t>(i)].constraint;
            const int room = sz[static_cast<std::size_t>(i)] - c.min;
            if (room <= 0) {
                continue;
            }
            const int take = room < deficit ? room : deficit;
            sz[static_cast<std::size_t>(i)] -= take;
            deficit -= take;
        }
    }

    // 5. 兜底：Σmin 本身超出 major 的病态情形，按父边界硬截断，
    //    子区域绝不越出父区域。
    {
        int off = 0;
        for (int i = 0; i < n; ++i) {
            const int room = major - off;
            if (sz[static_cast<std::size_t>(i)] > room) {
                sz[static_cast<std::size_t>(i)] = room > 0 ? room : 0;
            }
            off += sz[static_cast<std::size_t>(i)];
        }
    }

    // 赋区域：局部坐标。Rect 变化由 Widget::layout 自己检测并失效。
    int off = 0;
    for (int i = 0; i < n; ++i) {
        const int s = sz[static_cast<std::size_t>(i)];
        const Rect r = vert ? Rect{0, off, minor, s} : Rect{off, 0, s, minor};
        items_[static_cast<std::size_t>(i)].widget->layout(r);
        off += s;
    }
    // 尾部空隙 [off, major)：子项连续排满 [0, off)，这一段无人认领。
    // render 首次经过时清空一次即 disarm；未变化的纪元重复清空只是
    // 空白覆空白，差分输出零字节，可接受。
    const int slack = major - off > 0 ? major - off : 0;
    gap_ = vert ? Rect{0, off, minor, slack} : Rect{off, 0, slack, minor};
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

// 两条职责：
//   1. 一次性清空布局收缩后尾部腾出的空隙（gap_）：子项沿主轴连续
//      排布、副轴占满，未被覆盖的只可能是这一段。位置/尺寸变化的
//      子项已由 Widget::layout 标记失效、走增量路径自行重画；resize
//      纪元的全量补画由 L7 的 invalidate_tree() 负责，不在此重复。
//   2. 增量重画子树失效的子项：未失效且 Rect 未变的子项在 front 里
//      内容仍正确，back 起点又是 front 的拷贝 —— "只有转圈符号在动"
//      时每帧只碰一行。
void Container::render(Surface& s) {
    if (!gap_.empty()) {
        s.fill(gap_, U' ', Style{});
        gap_ = Rect{};
    }
    for (auto& it : items_) {
        Widget& w = *it.widget;
        if (!w.dirty_tree() || w.rect().empty()) {
            continue;
        }
        Surface v = s.view(w.rect());
        w.render(v);
        w.clear_dirty();
    }
    clear_dirty();
}

} // namespace dagent::tui
