// L3 布局 + 视图基类。
// Container(L3) 继承 Widget，为保持"依赖只向下"，Widget 基类并入本层，
// L4 的具体控件从本头派生。
// 焦点链与事件路由是 L6 的职责，本层不预留任何事件/焦点 API。
//
// 重画粒度由两个独立脏标记驱动：
//   dirty_        内容变了但尺寸没变 → 只需重画；
//   layout_dirty_ 内容变了且可能影响尺寸 → 需要重新布局（沿树聚合）。
#pragma once

#include <cstdint>
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

    // 自身的屏幕坐标矩形。
    Rect screen_rect() const noexcept {
        const Point o = screen_origin();
        return {o.x, o.y, rect_.w, rect_.h};
    }

    // 损伤补画（§3.4.2）：让屏幕矩形与 r 相交的控件失效。默认只看
    // 自己的 screen_rect()；Container/LayerStack 覆写为沿子项递归，
    // 保证被浮层盖过的深层控件也会补画。
    virtual void invalidate_rect(Rect r);

    // 屏幕坐标命中测试：返回包含该点的最深控件（§3.5 的命中链起点）。
    // 默认自身；容器覆写为子项逆序递归（后声明的画在上面）。
    virtual Widget* hit_test(Point screen) const noexcept;

    // 收集本帧实际重画的屏幕矩形（LayerStack 用它判断哪些浮层被基础
    // 层新画的内容盖住）。默认无；Container 覆写为记录实际渲染过的
    // 子项矩形。调用即清空本帧记录。
    virtual void take_painted(std::vector<Rect>& out);

    // 容器接收子项时登记父子关系（子项由容器拥有，生命周期被父覆盖）。
    static void adopt(Widget& parent, Widget& child) noexcept { child.parent_ = &parent; }
    // 摘除子项时解除父子关系（remove 返回的控件不再属于原树）。
    static void disown(Widget& child) noexcept { child.parent_ = nullptr; }

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

    void distribute(Rect area);

    std::vector<Item> items_;
    Direction dir_;
    // 本帧实际渲染过的子项屏幕矩形（含清空的 gap_）。render 开始时重置、
    // take_painted 取走后清空；只由 LayerStack 消费。
    std::vector<Rect> painted_;
    // 布局收缩后尾部腾出的区域（本容器局部坐标）。子项沿主轴连续
    // 排布、副轴占满，未被覆盖的只可能是 [off, major) 这一段；render
    // 首次经过时清空一次。resize 纪元的全量补画由 L7 调用
    // invalidate_tree() 负责（见 FrameOptions 注释），不在此重复。
    Rect gap_{};
};

// 浮层相对屏幕的摆放方式（§3.4.1）。
enum class Placement : uint8_t {
    center,      // 居中：对话框、命令面板
    top_right,   // 右上角：toast
    above_point, // 贴点上方，空间不足翻到下方（补全弹窗贴光标）
    at_point,    // 点处
};

// 层栈：一个基础层 + 按 z 序排列的浮层（§3.4）。作为根控件使用 ——
// 它的 rect 就是屏幕，因此浮层的矩形（及其子孙的 screen_origin()）
// 直接是屏幕坐标。
//
// 损伤传播（保留"干净控件跳过 + back 复制 front"）：
//   1. push/move/remove/重排把旧浮层矩形记入本帧损伤；
//   2. render 时把损伤区域擦成空白，并 invalidate_rect() 基础层中与
//      之交叠的控件（关闭/移动后下面的内容必须补画）；
//   3. 渲染基础层，take_painted() 汇总实际重画的子项矩形；
//   4. 按 z 序遍历浮层：自身失效或与 painted ∪ damage 相交 → 整层重画，
//      并把其矩形并入 painted（后画的浮层不会盖住先画的）。
class LayerStack : public Widget {
public:
    explicit LayerStack(std::unique_ptr<Widget> base);

    // 返回 id；尺寸取 overlay->measure(可用尺寸) 并夹到屏幕内。
    // 打开后浮层整体失效一次（首画）。
    uint32_t push(std::unique_ptr<Widget> overlay, Placement p,
                  Point point = {});
    // 重新摆放；矩形变化时旧矩形进入本帧损伤。
    void move(uint32_t id, Placement p, Point point = {});
    // 摘除浮层并归还所有权（不存在返回 nullptr）；旧矩形进入损伤。
    std::unique_ptr<Widget> remove(uint32_t id);
    // 最上层包含该点的最深控件（§3.5 命中入口）；无命中返回 nullptr。
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
        Rect rect{}; // 屏幕坐标
    };

    // measure + 摆放 + layout；矩形变化时把旧矩形记入损伤。
    void place(Overlay& ov);
    void damage(Rect r) noexcept;
    static bool intersects(const std::vector<Rect>& rs, Rect r) noexcept;

    std::unique_ptr<Widget> base_;
    std::vector<Overlay> overlays_; // 尾 = z 序最上
    std::vector<Rect> damage_;       // 本帧待补画的旧浮层矩形（屏幕坐标）
    std::vector<Rect> painted_;      // 复用容量：本帧基础层/低层浮层的重画矩形
    uint32_t next_id_ = 1;
};

} // namespace dagent::tui
