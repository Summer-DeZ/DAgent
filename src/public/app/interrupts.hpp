/// @file interrupts.hpp
/// @brief 前端进程级中断状态：POSIX 用 sigwait；Windows 用控制台回调和事件唤醒工作线程。
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
/// 继承屏蔽）；Windows 注册 Ctrl-C/Ctrl-Break 回调，首次优雅停止交给工作线程。
/// 只调用一次，返回的状态在进程内永久有效。POSIX 子进程的屏蔽由 exec 清空。
Interrupts& install_interrupts();

void ignore_broken_pipe();
void initialize_backend_signals();

} // namespace dagent::app
