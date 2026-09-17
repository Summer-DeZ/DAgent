// L3 布局 + 视图基类。
// Container(L3) 继承 Widget，为保持"依赖只向下"，Widget 基类并入本层，
// L4 的具体控件从本头派生。
// 焦点链与事件路由是 L6 的职责，本层不预留任何事件/焦点 API。
//
// 重画粒度由两个独立脏标记驱动：
//   dirty_        内容变了但尺寸没变 → 只需重画；
//   layout_dirty_ 内容变了且可能影响尺寸 → 需要重新布局（沿树聚合）。
#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "tui/surface.hpp"

namespace dagent::tui {

class Widget {
public:
    Widget() = default;
    virtual ~Widget() = default;
    // 控件以身份挂在树上，且实现可能持有指向自身成员的视图：禁止拷贝与移动。
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    // 在给定可用尺寸下自己想要多大（Sizing::content 时被布局调用）。
    virtual Size measure(Size available) const {
        (void)available;
        return {};
    }

    // 画到自己的 Surface 视图里。坐标系从 (0,0) 开始，越界被视图裁剪。
    // 实现不需要管理脏标记：容器在调用后统一清理。
    virtual void render(Surface&) = 0;

    virtual bool focusable() const { return false; }

    // 有焦点的 widget 指定光标落点（自身坐标系），渲染器帧末定位。
    virtual std::optional<Point> cursor() const { return std::nullopt; }

    // 内容变了但尺寸没变 → 只需重画。
    void invalidate() noexcept { dirty_ = true; }
    // 内容变了且可能影响尺寸 → 需要重新布局。
    void invalidate_layout() noexcept {
        dirty_ = true;
        layout_dirty_ = true;
    }
    // 整树强制重绘（主题变更、resize 后的补画）。
    virtual void invalidate_tree() noexcept { invalidate(); }

    void clear_dirty() noexcept { dirty_ = false; }
    bool dirty() const noexcept { return dirty_; }
    // 子树是否需要重画：容器聚合子孙的 invalidate，
    // 保证深层控件失效时父容器不会被渲染跳过。
    virtual bool dirty_tree() const noexcept { return dirty_; }
    // 布局纪元是否需要提升：容器聚合子孙的 invalidate_layout。
    virtual bool needs_layout() const noexcept { return layout_dirty_; }

    // 记录自己的区域（父容器坐标系）。Rect 任何变化都失效：内容是否
    // 仍然有效取决于控件怎么画（贴底、边框、折行），布局层无法判断。
    // 容器覆写以递归分配子区域。只应在纪元提升（终端尺寸变化 /
    // invalidate_layout）时被调用。
    virtual void layout(Rect area) {
        if (!(rect_ == area)) {
            rect_ = area;
            invalidate();
        }
        layout_dirty_ = false;
    }

    // 父容器坐标系下的区域。容器给子项分配的是局部坐标，
    // 需要屏幕位置（光标定位）时用 screen_origin()。
    Rect rect() const noexcept { return rect_; }

    // 左上角的屏幕坐标：沿父链累加各层局部偏移。O(嵌套深度)。
    Point screen_origin() const noexcept {
        Point p{rect_.x, rect_.y};
        for (const Widget* w = parent_; w != nullptr; w = w->parent_) {
            p.x += w->rect_.x;
            p.y += w->rect_.y;
        }
        return p;
    }

protected:
    // 容器接收子项时登记父子关系（子项由容器拥有，生命周期被父覆盖）。
    static void adopt(Widget& parent, Widget& child) noexcept { child.parent_ = &parent; }

    Rect rect_{};
    Widget* parent_ = nullptr; // 非拥有；根控件为空
    bool dirty_ = true;        // 首帧全量绘制
    bool layout_dirty_ = true;
};

enum class Sizing {
    fixed,   // 固定 N 行/列（value）
    content, // 由 measure() 决定，可为 0（不占位）
    flex,    // 分配剩余空间，按 value 为权重分摊（0 视为 1）
};

struct Constraint {
    Sizing sizing = Sizing::content;
    int value = 0;      // fixed 的行数，或 flex 的权重
    int min = 0;
    int max = 0x7FFFFFFF;
};

// 容器：vertical / horizontal 两个方向。布局算法（文档§六）：
//   1. fixed 直接占用；
//   2. content 调 measure(剩余空间)，夹到 [min, max]；
//   3. 剩余空间按权重分给 flex（floor + 余量按声明顺序补 1）；
//   4. 总需求超出可用空间（fixed/content 超订阅，或 flex 被 min 上顶）
//      时按声明顺序逆序压缩，直到 min；Σmin 仍超出时按父边界硬截断。
class Container : public Widget {
public:
    enum class Direction { vertical, horizontal };

    explicit Container(Direction dir = Direction::vertical) noexcept;

    void add(Constraint c, std::unique_ptr<Widget> w);
    int count() const noexcept { return static_cast<int>(items_.size()); }
    Widget& child(int i) const noexcept {
        return *items_[static_cast<std::size_t>(i)].widget;
    }

    void layout(Rect area) override;
    bool needs_layout() const noexcept override;
    bool dirty_tree() const noexcept override;
    void invalidate_tree() noexcept override;

    Size measure(Size available) const override;
    void render(Surface&) override;

private:
    struct Item {
        Constraint constraint;
        std::unique_ptr<Widget> widget;
    };

    void distribute(Rect area);

    std::vector<Item> items_;
    Direction dir_;
    // 布局收缩后尾部腾出的区域（本容器局部坐标）。子项沿主轴连续
    // 排布、副轴占满，未被覆盖的只可能是 [off, major) 这一段；render
    // 首次经过时清空一次。resize 纪元的全量补画由 L7 调用
    // invalidate_tree() 负责（见 FrameOptions 注释），不在此重复。
    Rect gap_{};
};

} // namespace dagent::tui
