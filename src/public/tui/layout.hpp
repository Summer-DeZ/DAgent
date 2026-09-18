/// @file layout.hpp
/// @brief 布局与视图基类：measure / layout / render 契约与脏标记。
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "tui/surface.hpp"

namespace dagent::tui {

/// @brief 视图基类。
class Widget {
public:
    Widget() = default;
    virtual ~Widget() = default;
    /// @note 控件以身份挂在树上，禁止拷贝与移动。
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    /// @brief 给定可用尺寸下期望的尺寸（Sizing::content 时由容器调用）。
    virtual Size measure(Size available) const {
        (void)available;
        return {};
    }

    /// @brief 画到自己的 Surface 视图里（坐标系从 (0,0) 开始）。
    virtual void render(Surface&) = 0;

    virtual bool focusable() const { return false; }

    /// @brief 光标落点（自身坐标系）；无光标时为空。
    virtual std::optional<Point> cursor() const { return std::nullopt; }

    /// @brief 标记需要重画。
    void invalidate() noexcept { dirty_ = true; }
    /// @brief 标记需要重新布局（也会重画）。
    void invalidate_layout() noexcept {
        dirty_ = true;
        layout_dirty_ = true;
    }
    /// @brief 整树强制重画。
    virtual void invalidate_tree() noexcept { invalidate(); }

    void clear_dirty() noexcept { dirty_ = false; }
    bool dirty() const noexcept { return dirty_; }
    /// @brief 子树是否有需要重画的控件（容器聚合）。
    virtual bool dirty_tree() const noexcept { return dirty_; }
    /// @brief 布局纪元是否已提升（容器聚合）。
    virtual bool needs_layout() const noexcept { return layout_dirty_; }

    /// @brief 记录自己的区域（父容器坐标系）；容器覆写以分配子区域。
    virtual void layout(Rect area) {
        if (!(rect_ == area)) {
            rect_ = area;
            invalidate();
        }
        layout_dirty_ = false;
    }

    /// @brief 父容器坐标系下的区域。
    Rect rect() const noexcept { return rect_; }

    /// @brief 父控件；根控件为 nullptr。
    Widget* parent() const noexcept { return parent_; }

    /// @brief 左上角的屏幕坐标。
    Point screen_origin() const noexcept {
        Point p{rect_.x, rect_.y};
        for (const Widget* w = parent_; w != nullptr; w = w->parent_) {
            p.x += w->rect_.x;
            p.y += w->rect_.y;
        }
        return p;
    }

    /// @brief 自身的屏幕矩形。
    Rect screen_rect() const noexcept {
        const Point o = screen_origin();
        return {o.x, o.y, rect_.w, rect_.h};
    }

    /// @brief 让屏幕矩形与 r 相交的控件失效；容器覆写为递归子项。
    virtual void invalidate_rect(Rect r);

    /// @brief 返回包含该点的最深控件；无命中返回 nullptr。
    virtual Widget* hit_test(Point screen) const noexcept;

    /// @brief 取走本帧实际重画的屏幕矩形；调用即清空。
    virtual void take_painted(std::vector<Rect>& out);

    /// @brief 登记父子关系（子项生命周期由父容器持有）。
    static void adopt(Widget& parent, Widget& child) noexcept { child.parent_ = &parent; }
    /// @brief 解除父子关系。
    static void disown(Widget& child) noexcept { child.parent_ = nullptr; }

    Rect rect_{};
    Widget* parent_ = nullptr; ///< 非拥有
    bool dirty_ = true;        ///< 需要重画
    bool layout_dirty_ = true; ///< 需要重新布局
};

/// @brief 子项尺寸策略。
enum class Sizing {
    fixed,   ///< 固定 N 行/列（value）
    content, ///< 由 measure() 决定，可为 0
    flex,    ///< 按 value 权重分摊剩余空间（0 视为 1）
};

/// @brief 单个子项的尺寸约束。
struct Constraint {
    Sizing sizing = Sizing::content;
    int value = 0;      ///< fixed 的行数，或 flex 的权重
    int min = 0;
    int max = 0x7FFFFFFF;
};

/// @brief 沿主轴排列子项的容器。
class Container : public Widget {
public:
    /// @brief 主轴方向。
    enum class Direction { vertical, horizontal };

    explicit Container(Direction dir = Direction::vertical) noexcept;

    /// @brief 按约束追加子项。
    void add(Constraint c, std::unique_ptr<Widget> w);
    int count() const noexcept { return static_cast<int>(items_.size()); }
    Widget& child(int i) const noexcept {
        return *items_[static_cast<std::size_t>(i)].widget;
    }

    void layout(Rect area) override;
    bool needs_layout() const noexcept override;
    bool dirty_tree() const noexcept override;
    void invalidate_tree() noexcept override;
    void invalidate_rect(Rect r) override;
    Widget* hit_test(Point screen) const noexcept override;
    void take_painted(std::vector<Rect>& out) override;

    Size measure(Size available) const override;
    void render(Surface&) override;

private:
    struct Item {
        Constraint constraint;
        std::unique_ptr<Widget> widget;
    };

    /// @brief 按约束把 area 分配给子项。
    void distribute(Rect area);

    std::vector<Item> items_;
    Direction dir_;
    std::vector<Rect> painted_; ///< 本帧实际渲染过的子项屏幕矩形
    Rect gap_{}; ///< 布局收缩后尾部腾出的区域；render 时清空一次
};

/// @brief 浮层相对屏幕的摆放方式。
enum class Placement : uint8_t {
    center,      ///< 居中
    top_right,   ///< 右上角
    above_point, ///< 贴点上方，空间不足翻到下方
    at_point,    ///< 点处
};

/// @brief 层栈：一个基础层 + 按 z 序排列的浮层，用作根控件。
class LayerStack : public Widget {
public:
    explicit LayerStack(std::unique_ptr<Widget> base);

    /// @brief 打开浮层并返回 id；尺寸取 measure 结果并夹到屏幕内。
    uint32_t push(std::unique_ptr<Widget> overlay, Placement p,
                  Point point = {});
    /// @brief 重新摆放浮层。
    void move(uint32_t id, Placement p, Point point = {});
    /// @brief 摘除浮层并归还所有权；不存在返回 nullptr。
    std::unique_ptr<Widget> remove(uint32_t id);
    /// @brief 最上层包含该点的最深控件；无命中返回 nullptr。
    Widget* hit(Point screen) const noexcept;

    void layout(Rect area) override;
    bool needs_layout() const noexcept override;
    bool dirty_tree() const noexcept override;
    void invalidate_tree() noexcept override;
    void invalidate_rect(Rect r) override;
    Size measure(Size available) const override;
    void render(Surface& s) override;
    Widget* hit_test(Point screen) const noexcept override;

private:
    struct Overlay {
        uint32_t id = 0;
        std::unique_ptr<Widget> widget;
        Placement placement = Placement::center;
        Point point{};
        Rect rect{}; ///< 屏幕坐标
    };

    /// @brief measure + 摆放 + layout；矩形变化时把旧矩形记入损伤。
    void place(Overlay& ov);
    void damage(Rect r) noexcept;
    static bool intersects(const std::vector<Rect>& rs, Rect r) noexcept;

    std::unique_ptr<Widget> base_;
    std::vector<Overlay> overlays_; ///< 尾 = z 序最上
    std::vector<Rect> damage_;       ///< 本帧待补画的旧浮层矩形
    std::vector<Rect> painted_;      ///< 本帧基础层/低层浮层的重画矩形
    uint32_t next_id_ = 1;
};

} // namespace dagent::tui
