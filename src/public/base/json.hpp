/// @file json.hpp
/// @brief JSON 公共规则：模型调用参数解析与写盘/日志脱敏。
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <span>
#include <string>

#include "lib/nlohmann/json.hpp"

namespace dagent::base {

// ---------------------------------------------------------------- 参数解析

/// @brief 把模型给的 arguments 变成 JSON 对象：空串按 {}；坏 JSON 返回带 nlohmann 报错位置的信息。
std::expected<nlohmann::json, std::string> parse_arguments(std::string_view arguments);

/// @brief 必填字符串字段；缺失、类型错（含宽容字符串数字）、空串时写 err 并返回空。
std::string require_string(const nlohmann::json& args, std::string_view key, std::string& err);

/// @brief 可选字符串字段；类型错时写 err。
std::optional<std::string> get_string(const nlohmann::json& args, std::string_view key, std::string& err);

/// @brief 可选整数字段。宽容一种常见错误：收到字符串 "10" 时照常接受；其他类型写 err。
std::optional<std::int64_t> get_int(const nlohmann::json& args, std::string_view key, std::string& err);

/// @brief 可选布尔字段。宽容 "true"/"false" 字符串；其他类型写 err。
std::optional<bool> get_bool(const nlohmann::json& args, std::string_view key, std::string& err);



/// @brief 递归处理对象和数组，把键名（忽略大小写）在 fields 里的值替换成 "***"。
void redact(nlohmann::json& j, std::span<const std::string> fields);

} // namespace dagent::base
