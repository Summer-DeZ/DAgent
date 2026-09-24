/// @file interrupts.hpp
/// @brief 前端进程级中断状态：SIGINT/SIGTERM 由 sigwait 线程处理。
///
/// 可优雅结束期间第一个信号请求 stop，第二个信号直接 _Exit(130)；启动/收尾等轮外阶段直接退出。
/// 语义与 docs/design/app.md §6 一致。
#pragma once

#include <atomic>
#include <stop_token>

namespace dagent::app {

struct Interrupts {
    std::stop_source stop;
    std::atomic<bool> graceful{false};
};

/// @brief 屏蔽 SIGINT/SIGTERM 并启动 sigwait 线程。必须在进程创建任何线程之前调用（之后创建的线程都
/// 继承屏蔽）；只调用一次，返回的状态在进程内永久有效。子进程的屏蔽由 exec 清空。
Interrupts& install_interrupts();

} // namespace dagent::app
