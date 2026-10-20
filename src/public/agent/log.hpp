/// @file log.hpp
/// @brief Agent 调度与授权共用的具名日志入口。
#pragma once

#include "base/log.hpp"

namespace dagent::agent {

inline std::shared_ptr<spdlog::logger> log_agent() { return base::logger("agent"); }

} // namespace dagent::agent
