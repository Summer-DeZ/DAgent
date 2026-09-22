#pragma once

#include <string>
#include <vector>

#include "agent/events.hpp"
#include "agent/headless.hpp"
#include "runtime/controller.hpp"
#include "runtime/runtime.hpp"

namespace dagent::ui {

struct InteractiveOptions {
    std::string initial_prompt;
    bool resumed = false;
    std::vector<agent::Event> replay; ///< 启动/恢复时后端附加的通知（不含持久历史）
};

/// 进入全屏前，后端 Runtime 已完成创建/恢复；Interrupts 把进程信号桥接到界面退出。
int run_interactive(runtime::Runtime&, const InteractiveOptions&, agent::Interrupts&);

} // namespace dagent::ui
