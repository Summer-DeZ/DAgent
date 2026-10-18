/// @file decision_profile.hpp
/// @brief 加载与决策模型完整版本绑定的阈值配置。
#pragma once

#include <filesystem>

#include "agent/decision_types.hpp"

namespace dagent::app {

/// @brief 读取显式场景阈值；身份不匹配或格式错误抛 ConfigError，不继承其他版本。
agent::ThresholdProfile load_decision_profile(const std::filesystem::path&,
                                              const agent::ReviewVersion&);

} // namespace dagent::app
