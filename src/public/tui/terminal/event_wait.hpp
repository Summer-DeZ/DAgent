/// @file event_wait.hpp
/// @brief 终端输入、信号与跨线程唤醒的等待；事件消费顺序由 Runtime 控制。
#pragma once

#include <cstddef>
#include <memory>
#include <span>

namespace dagent::tui {

class Terminal;

class EventWait {
public:
    /// @brief 借用终端；Terminal 必须晚于本对象销毁。
    explicit EventWait(Terminal& terminal);
    ~EventWait();
    EventWait(const EventWait&) = delete;
    EventWait& operator=(const EventWait&) = delete;

    struct Ready {
        enum class Status { completed, interrupted, failed };
        Status status;
        bool signal = false;
        bool wake = false;
        bool input = false;
    };

    struct Input {
        std::size_t size = 0;
        bool closed = false;
    };

    /// @brief 只返回就绪状态，不消费事件；负超时表示无限等待。
    Ready wait(int timeout_ms) noexcept;
    Input read_input(std::span<char> bytes) noexcept;
    void wake() noexcept;
    void drain_wake() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dagent::tui
