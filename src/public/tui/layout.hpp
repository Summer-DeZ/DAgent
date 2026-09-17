// L3 布局：区域树 + 尺寸策略。故意做得小：固定 + 内容 + 弹性三种策略
// 覆盖纵向堆叠式界面的全部需要，不做约束求解器。
#pragma once

#include <memory>
#include <vector>

#include "tui/widget.hpp"

namespace dagent::tui {

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

// 容器：vertical / horizontal 两个方向。布局算法：
//   1. fixed 直接占用；
//   2. content 调 measure(剩余空间)，按 min/max 夹取；
//   3. 剩余空间按权重分给 flex（floor + 余量按声明顺序补 1）；
//   4. 不够分（超订阅）时按声明顺序逆序压缩，直到 min。
class Container : public Widget {
public:
    enum class Direction { vertical, horizontal };

    explicit Container(Direction dir = Direction::vertical) noexcept;

    void add(Constraint c, std::unique_ptr<Widget> w);
    int count() const noexcept { return static_cast<int>(items_.size()); }
    Widget& child(int i) const noexcept { return *items_[i].widget; }
    const Constraint& constraint(int i) const noexcept {
        return items_[i].constraint;
    }

    void layout(Rect area) override;
    bool needs_layout() const noexcept override;
    void invalidate_tree() noexcept override;

    Size measure(Size available) const override;
    void render(Surface&) override;
    bool on_event(const Event&) override;
    std::optional<Point> cursor() const override;

    // 焦点在可聚焦子项间循环；切换时新旧子项都失效（光标/选区重画）。
    bool focus_next() noexcept;
    bool focus_prev() noexcept;
    int focus_index() const noexcept { return focus_; }

private:
    struct Item {
        Constraint constraint;
        std::unique_ptr<Widget> widget;
    };

    void distribute(Rect area);
    bool move_focus(int step) noexcept;

    std::vector<Item> items_;
    Direction dir_;
    int focus_ = -1; // 无焦点
};

} // namespace dagent::tui
