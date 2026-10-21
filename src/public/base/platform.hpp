/// @file platform.hpp
/// @brief 跨模块共用的平台原语；路径策略、格式化与业务错误由调用方处理。
#pragma once

#include <cstdint>
#include <ctime>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace dagent::base {

std::filesystem::path executable_path();
std::filesystem::path executable_path(std::error_code& error);

/// @brief 返回环境变量的独立副本；未设置为 nullopt，已设置为空为有值的空字符串，不缓存。
std::optional<std::string> environment_variable(std::string_view name);
/// @brief 返回用户 Home 文本的独立副本；POSIX 只读取 HOME，保留缺失与空值的区别。
std::optional<std::string> home_directory();
std::int64_t process_id() noexcept;
bool stdin_is_terminal() noexcept;
bool stdout_is_terminal() noexcept;

/// @brief 写入调用方的 tm，返回转换是否成功；不改变调用方的失败处理。
bool local_time(std::time_t value, std::tm& result) noexcept;
bool utc_time(std::time_t value, std::tm& result) noexcept;
std::time_t utc_timestamp(std::tm& value) noexcept;

/// @brief 检查执行权限，不附加普通文件判断。
bool can_execute(const std::filesystem::path& path) noexcept;

/// @brief 返回默认临时目录；POSIX 固定为 /tmp，不读取 TMPDIR 等环境变量。
std::filesystem::path default_temporary_directory();

/// @brief 按完整的 XXXXXX 模板创建独占目录，不创建父目录、不自动清理。
/// 失败返回系统错误文本，由调用方补充业务上下文。
std::expected<std::filesystem::path, std::string> create_unique_directory(const std::filesystem::path& pattern);

} // namespace dagent::base
