/// @file headless.hpp
/// @brief run 模式：非交互跑完一轮，按格式输出结果后退出。
#pragma once

#include <atomic>
#include <optional>
#include <stop_token>
#include <string>

#include "agent/options.hpp"

namespace dagent::agent {

struct HeadlessOptions {
    enum class Output { text, json, jsonl };
    Output output = Output::text;
    std::string prompt; ///< app 已经合并了 stdin
    std::optional<std::string> resume_id;
    bool continue_last = false;
};

/// @brief 进程级的中断状态（docs/design/agent.md §12）。本轮运行中（graceful 为 true）第一次 SIGINT/SIGTERM 只
/// request_stop，第二次 _Exit(130)；轮外（启动、读 stdin、收尾）收到信号直接 _Exit(130)。
struct Interrupts {
    std::stop_source stop;
    std::atomic<bool> graceful{false};
};

/// @brief 屏蔽 SIGINT/SIGTERM 并启动 sigwait 线程。必须在进程创建任何线程之前调用（之后创建的线程都
/// 继承屏蔽）；只调用一次，返回的状态在进程内永久有效。子进程的屏蔽由 exec 清空。
Interrupts& install_interrupts();

/// @brief 装配好的 Setup + 提示词 → 跑完一轮。返回进程退出码。
int run_headless(Setup setup, const HeadlessOptions&, Interrupts&);

} // namespace dagent::agent
