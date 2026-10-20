/// @file path.hpp
/// @brief 路径规范化及文件系统错误时的回退。
#pragma once

#include <filesystem>

namespace dagent::base {

/// @brief 优先 weakly_canonical：解析已存在部分的符号链接，并规范化剩余路径。
/// 若失败，使用 absolute 后做 lexically_normal；若 absolute 也失败，
/// 返回原路径的 lexically_normal，此时结果可能仍是相对路径。
/// 文件系统操作使用 error_code 重载；不捕获内存分配等异常。
std::filesystem::path canonical_or_absolute_or_lexical(const std::filesystem::path& path);

} // namespace dagent::base
