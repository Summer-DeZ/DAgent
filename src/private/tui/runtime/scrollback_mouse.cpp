#include "tui/runtime/scrollback_mouse.hpp"

#include <optional>

namespace dagent::tui {

ScrollbackMouse::~ScrollbackMouse() {
    if (click_timer_ != 0) rt_.cancel(click_timer_);
}

bool ScrollbackMouse::on_event(const Event& e) {
    if (e.kind != Event::Kind::mouse) return false;
    const Event::Mouse& m = e.mouse;
    if (m.button == 4 || m.button == 5) { // 滚轮
        sb_.scroll_lines(m.button == 4 ? -3 : 3);
        return true;
    }
    if (m.button != 0) return false;
    const Point at{m.x, m.y};

    if (m.press && !m.motion) {
        // 连击：窗口内同一格再按记为多击。
        clicks_ = click_timer_ != 0 && at == last_press_ ? clicks_ + 1 : 1;
        last_press_ = at;
        if (click_timer_ != 0) rt_.cancel(click_timer_);
        click_timer_ = rt_.after(k_multi_click, [this] {
            click_timer_ = 0;
            clicks_ = 0;
        });
        const std::optional<Location> loc = sb_.hit(at);
        dragging_ = false;
        if (!loc) return true;
        if (clicks_ == 1) {
            press_ = *loc;
            dragging_ = true;
            sb_.clear_selection();
        } else if (clicks_ == 2) {
            sb_.select(sb_.document().word_around(*loc));
        } else {
            sb_.select(sb_.document().line_around(*loc));
        }
        return true;
    }
    if (m.press && m.motion) { // 拖拽（捕获中，坐标可能在控件外）
        if (!dragging_) return true;
        if (const std::optional<Location> loc = sb_.hit(at)) {
            sb_.select({press_, *loc});
        }
        return true;
    }
    // 释放：复制选区。
    dragging_ = false;
    if (copy_on_release && sb_.selection()) rt_.set_clipboard(sb_.selected_text());
    return true;
}

} // namespace dagent::tui
