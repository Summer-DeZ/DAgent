/// @file run.hpp
/// @brief run 模式前端：启动本前端独占的后端，经协议完成一轮并输出旧格式结果。
#pragma once

#include <string>

#include "app/cli.hpp"
#include "app/interrupts.hpp"
#include "app/launcher.hpp"

namespace dagent::app {

/// @brief 提交提示词、消费事件并按原 stdout/stderr/退出码规则收尾；返回进程退出码。
int run_backend(const BackendLaunch& launch, std::string prompt, OutputFormat output,
                Interrupts& interrupts);

} // namespace dagent::app
