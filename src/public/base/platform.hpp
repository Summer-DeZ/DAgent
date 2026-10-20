/// @file platform.hpp
/// @brief 跨模块共用的平台原语；路径策略、格式化与业务错误由调用方处理。
#pragma once

#include <cstdint>
#include <ctime>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>

namespace dagent::base {

std::filesystem::path executable_path();
std::filesystem::path executable_path(std::error_code& error);

/// @brief 借用 HOME 环境变量；保留未设置（nullptr）与空字符串的区别，不缓存。
const char* home_directory() noexcept;
std::int64_t process_id() noexcept;
bool stdin_is_terminal() noexcept;

/// @brief 写入调用方的 tm，返回转换是否成功；不改变调用方的失败处理。
bool local_time(std::time_t value, std::tm& result) noexcept;
bool utc_time(std::time_t value, std::tm& result) noexcept;
std::time_t utc_timestamp(std::tm& value) noexcept;

/// @brief 检查执行权限，不附加普通文件判断。
bool can_execute(const std::filesystem::path& path) noexcept;

/// @brief 按完整的 XXXXXX 模板创建独占目录，不创建父目录、不自动清理。
/// 失败返回系统错误文本，由调用方补充业务上下文。
std::expected<std::filesystem::path, std::string> create_unique_directory(std::string pattern);

} // namespace dagent::base
