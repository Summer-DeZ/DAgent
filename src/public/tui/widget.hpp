// L4 视图基类。L3 的 Container、L5 的滚动区都是普通 Widget。
// 重画粒度由两个独立脏标记驱动：
//   dirty_        内容变了但尺寸没变 → 只需重画；
//   layout_dirty_ 内容变了且可能影响尺寸 → 需要重新布局（沿树聚合）。
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "tui/surface.hpp"

namespace dagent::tui {

// 修饰键位。解码（L6）产生，视图层只消费。
enum class Mods : uint16_t {
    none  = 0,
    shift = 1 << 0,
    ctrl  = 1 << 1,
    alt   = 1 << 2,
};

constexpr Mods operator|(Mods a, Mods b) noexcept {
    return static_cast<Mods>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
constexpr Mods operator&(Mods a, Mods b) noexcept {
    return static_cast<Mods>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}
constexpr bool any(Mods m) noexcept { return static_cast<uint16_t>(m) != 0; }

// 语义键。文本输入走 Event::text，不在这里。
enum class Key : uint8_t {
    none, enter, tab, backspace, delete_, escape,
    up, down, left, right, home, end, page_up, page_down, insert,
    f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
};

struct Event {
    enum class Kind { text, key, mouse, paste, resize, focus };
    Kind kind = Kind::key;
    std::string text;   // text/paste 的内容（UTF-8）
    Key key = Key::none;
    Mods mods = Mods::none;
    struct {
        int button = 0;
        int col = 0;
        int row = 0;
        bool press = false;
        bool motion = false;
    } mouse;
};

class Widget {
public:
    virtual ~Widget() = default;

    // 在给定可用尺寸下自己想要多大（Sizing::content 时被布局调用）。
    virtual Size measure(Size available) const { (void)available; return {}; }

    // 画到自己的 Surface 视图里。坐标系从 (0,0) 开始，越界被视图裁剪。
    virtual void render(Surface&) = 0;

    // 返回 true 表示已消费，事件不再下沉。
    virtual bool on_event(const Event&) { return false; }

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
    // 整树强制重绘（主题变更、全屏刷新）。
    virtual void invalidate_tree() noexcept { invalidate(); }

    virtual bool needs_layout() const noexcept { return layout_dirty_; }
    // 布局纪元触发时由容器调用：分配自己的区域。叶子直接占用。
    virtual void layout(Rect area) { rect_ = area; }

    Rect rect() const noexcept { return rect_; }
    bool dirty() const noexcept { return dirty_; }

private:
    friend class Container;

    Rect rect_{};
    bool dirty_ = true;        // 首帧全量绘制
    bool layout_dirty_ = true;
};

} // namespace dagent::tui
