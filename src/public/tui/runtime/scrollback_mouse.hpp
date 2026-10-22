/// @file scrollback_mouse.hpp
/// @brief 滚动区鼠标处理：拖拽选择、多击选词选行、滚轮滚动与释放复制。
#pragma once

#include <chrono>

#include "tui/document/document.hpp"
#include "tui/runtime/runtime.hpp"

namespace dagent::tui {

/// @brief 滚动区鼠标处理：左键拖拽选择、双击选词、三击选行、滚轮滚动；
/// 释放时把选区写入剪贴板（copy_on_release 控制）。
/// @note 必须先于 Runtime 与 Scrollback 销毁。
class ScrollbackMouse : public EventHandler {
public:
    static constexpr std::chrono::milliseconds k_multi_click{400};

    ScrollbackMouse(Runtime& rt, Scrollback& sb) noexcept : rt_(rt), sb_(sb) {}
    ~ScrollbackMouse() override;
    ScrollbackMouse(const ScrollbackMouse&) = delete;
    ScrollbackMouse& operator=(const ScrollbackMouse&) = delete;

    bool copy_on_release = true;

    bool on_event(const Event& e) override;

private:
    Runtime& rt_;
    Scrollback& sb_;
    TimerId click_timer_ = 0; ///< 连击窗口计时器
    int clicks_ = 0;
    Point last_press_{-1, -1};
    Location press_{};      ///< 拖拽起点
    bool dragging_ = false;
};

} // namespace dagent::tui
