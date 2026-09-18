#include "tui/layout.hpp"

#include <algorithm>
#include <utility>

namespace dagent::tui {

namespace {

int clampv(int v, int lo, int hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// 浮层矩形：尺寸已夹到屏幕内，这里再保证位置不越界。
Rect placement_rect(Placement p, Size size, Point point, Size avail) noexcept {
    const int w = size.cols;
    const int h = size.rows;
    int x = 0;
    int y = 0;
    switch (p) {
    case Placement::center:
        x = (avail.cols - w) / 2;
        y = (avail.rows - h) / 2;
        break;
    case Placement::top_right:
        x = avail.cols - w;
        break;
    case Placement::above_point:
        x = point.x;
        y = point.y - h;
        if (y < 0) y = point.y; // 上方空间不足：翻到点的下方
        break;
    case Placement::at_point:
        x = point.x;
        y = point.y;
        break;
    }
    const int max_x = avail.cols - w;
    const int max_y = avail.rows - h;
    x = clampv(x, 0, max_x > 0 ? max_x : 0);
    y = clampv(y, 0, max_y > 0 ? max_y : 0);
    return {x, y, w, h};
}

} // namespace

// ---- Widget 默认实现 ----

void Widget::invalidate_rect(Rect r) {
    if (!screen_rect().intersect(r).empty()) invalidate();
}

Widget* Widget::hit_test(Point screen) const noexcept {
    return screen_rect().contains(screen) ? const_cast<Widget*>(this) : nullptr;
}

void Widget::take_painted(std::vector<Rect>&) {}

Container::Container(Direction dir) noexcept : dir_(dir) {}

void Container::add(Constraint c, std::unique_ptr<Widget> w) {
    adopt(*this, *w);
    items_.push_back({c, std::move(w)});
    invalidate_layout();
}

void Container::invalidate_tree() noexcept {
    invalidate();
    for (auto& it : items_) {
        it.widget->invalidate_tree();
    }
}

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
    // 子区域是局部坐标：Rect 不变也可能屏幕位置已变，整棵子树都得重画。
    const bool changed = !(rect_ == area);
    Widget::layout(area);
    distribute(area);
    if (changed) {
        invalidate_tree();
    }
}

void Container::invalidate_rect(Rect r) {
    for (auto& it : items_) {
        it.widget->invalidate_rect(r);
    }
}

Widget* Container::hit_test(Point screen) const noexcept {
    // 逆序：后声明的子项画在上面。
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
        if (Widget* w = it->widget->hit_test(screen)) return w;
    }
    return Widget::hit_test(screen);
}

void Container::take_painted(std::vector<Rect>& out) {
    out.insert(out.end(), painted_.begin(), painted_.end());
    painted_.clear();
}

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

    // 2. content 按声明顺序测量，夹到 [min, max]。
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
            // floor 损失的余量按声明顺序补 1
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
            // 剩余空间 ≤ 0：flex 按 min 占位。
            for (int i : flex_idx) {
                const Constraint& c =
                    items_[static_cast<std::size_t>(i)].constraint;
                sz[static_cast<std::size_t>(i)] = clampv(0, c.min, c.max);
            }
        }
    }

    // 4. 收缩步：总需求超出 major 时按声明顺序逆序压缩到 min。
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

    // 5. 兜底：子区域按父边界硬截断。
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

    // 赋子区域：局部坐标，Rect 变化由 Widget::layout 检测失效。
    int off = 0;
    for (int i = 0; i < n; ++i) {
        const int s = sz[static_cast<std::size_t>(i)];
        const Rect r = vert ? Rect{0, off, minor, s} : Rect{off, 0, s, minor};
        items_[static_cast<std::size_t>(i)].widget->layout(r);
        off += s;
    }
    // 尾部空隙无人认领，记入 gap_ 待 render 清空。
    const int slack = major - off > 0 ? major - off : 0;
    gap_ = vert ? Rect{0, off, minor, slack} : Rect{off, 0, slack, minor};
}

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

/// @brief 清空尾部空隙（gap_），增量重画失效子项。
void Container::render(Surface& s) {
    painted_.clear();
    if (!gap_.empty()) {
        s.fill(gap_, U' ', Style{});
        const Point o = screen_origin();
        painted_.push_back({o.x + gap_.x, o.y + gap_.y, gap_.w, gap_.h});
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
        // 记录实际重画的子项屏幕矩形。
        painted_.push_back(w.screen_rect());
    }
    clear_dirty();
}

// ---- LayerStack ----

LayerStack::LayerStack(std::unique_ptr<Widget> base) : base_(std::move(base)) {
    adopt(*this, *base_);
}

uint32_t LayerStack::push(std::unique_ptr<Widget> overlay, Placement p,
                          Point point) {
    adopt(*this, *overlay);
    const uint32_t id = next_id_++;
    overlays_.push_back({id, std::move(overlay), p, point, {}});
    Overlay& ov = overlays_.back();
    place(ov);
    ov.widget->invalidate_tree(); // 新浮层首画
    return id;
}

void LayerStack::move(uint32_t id, Placement p, Point point) {
    for (auto& ov : overlays_) {
        if (ov.id != id) continue;
        ov.placement = p;
        ov.point = point;
        place(ov);
        return;
    }
}

std::unique_ptr<Widget> LayerStack::remove(uint32_t id) {
    for (auto it = overlays_.begin(); it != overlays_.end(); ++it) {
        if (it->id != id) continue;
        damage(it->rect);
        std::unique_ptr<Widget> w = std::move(it->widget);
        overlays_.erase(it);
        disown(*w);
        return w;
    }
    return nullptr;
}

Widget* LayerStack::hit(Point screen) const noexcept { return hit_test(screen); }

void LayerStack::layout(Rect area) {
    // 根控件：area 即屏幕，浮层矩形因此就是屏幕坐标。
    Widget::layout(area);
    base_->layout(area);
    for (auto& ov : overlays_) {
        place(ov);
    }
}

bool LayerStack::needs_layout() const noexcept {
    if (layout_dirty_ || base_->needs_layout()) return true;
    for (const auto& ov : overlays_) {
        if (ov.widget->needs_layout()) return true;
    }
    return false;
}

bool LayerStack::dirty_tree() const noexcept {
    if (dirty_ || !damage_.empty() || base_->dirty_tree()) return true;
    for (const auto& ov : overlays_) {
        if (ov.widget->dirty_tree()) return true;
    }
    return false;
}

void LayerStack::invalidate_tree() noexcept {
    invalidate();
    base_->invalidate_tree();
    for (auto& ov : overlays_) {
        ov.widget->invalidate_tree();
    }
}

void LayerStack::invalidate_rect(Rect r) {
    base_->invalidate_rect(r);
    for (auto& ov : overlays_) {
        ov.widget->invalidate_rect(r);
    }
}

Size LayerStack::measure(Size available) const { return base_->measure(available); }

Widget* LayerStack::hit_test(Point screen) const noexcept {
    // 浮层逆序：后 push 的在上面；都没有命中的则落回基础层。
    for (auto it = overlays_.rbegin(); it != overlays_.rend(); ++it) {
        if (Widget* w = it->widget->hit_test(screen)) return w;
    }
    return base_->hit_test(screen);
}

/// @brief 损伤矩形擦空白并传给相交子树，再逐层重画。
void LayerStack::render(Surface& s) {
    for (const Rect d : damage_) {
        s.fill(d, U' ', Style{});
        base_->invalidate_rect(d);
    }

    painted_.clear();
    const bool base_dirty = base_->dirty_tree();
    if (base_dirty && !base_->rect().empty()) {
        Surface v = s.view(base_->rect());
        base_->render(v);
        base_->clear_dirty();
        base_->take_painted(painted_);
        if (painted_.empty()) painted_.push_back(base_->screen_rect());
    }

    for (auto& ov : overlays_) {
        if (ov.rect.empty()) {
            ov.widget->clear_dirty();
            continue;
        }
        const bool dirty = ov.widget->dirty_tree();
        const bool covered = intersects(damage_, ov.rect) ||
                             intersects(painted_, ov.rect);
        if (!dirty && !covered) continue;
        if (covered) ov.widget->invalidate_tree(); // 被盖住：整层重画
        Surface v = s.view(ov.rect);
        ov.widget->render(v);
        ov.widget->clear_dirty();
        painted_.push_back(ov.rect);
    }

    damage_.clear();
    clear_dirty();
}

void LayerStack::place(Overlay& ov) {
    const Size avail{rect_.w, rect_.h};
    Size size = ov.widget->measure(avail);
    size.cols = clampv(size.cols, 0, avail.cols > 0 ? avail.cols : 0);
    size.rows = clampv(size.rows, 0, avail.rows > 0 ? avail.rows : 0);
    const Rect r = placement_rect(ov.placement, size, ov.point, avail);
    if (r == ov.rect) {
        if (ov.widget->needs_layout()) ov.widget->layout(r);
        return;
    }
    damage(ov.rect);
    ov.rect = r;
    ov.widget->layout(r);
}

void LayerStack::damage(Rect r) noexcept {
    if (!r.empty()) damage_.push_back(r);
}

bool LayerStack::intersects(const std::vector<Rect>& rs, Rect r) noexcept {
    for (const Rect& x : rs) {
        if (!x.intersect(r).empty()) return true;
    }
    return false;
}

} // namespace dagent::tui
