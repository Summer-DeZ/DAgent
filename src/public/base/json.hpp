/// @file json.hpp
/// @brief JSON 脱敏：写盘或记日志之前，把密钥类字段的值替换掉。
#pragma once

#include <span>
#include <string>

#include "lib/nlohmann/json.hpp"

namespace dagent::base {

/// @brief 递归处理对象和数组，把键名（忽略大小写）在 fields 里的值替换成 "***"。
void redact(nlohmann::json& j, std::span<const std::string> fields);

} // namespace dagent::base
