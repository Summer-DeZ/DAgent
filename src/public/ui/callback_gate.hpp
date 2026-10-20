#pragma once

#include <mutex>

namespace dagent::ui {

/// @brief 异步回调的寿命闸门：Shell 析构后回调直接丢弃，不再触碰页面或 Runtime。
struct CallbackGate {
    std::mutex mutex;
    bool open = true;
};

} // namespace dagent::ui
