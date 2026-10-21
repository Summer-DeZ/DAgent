/// @file path.hpp
/// @brief 路径文本转换、规范化及文件系统错误时的回退。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace dagent::base {

/// @brief 路径与 UTF-8 文本的边界；POSIX 保留原始字节，不校验或修复非法 UTF-8。
/// 不进行路径规范化、绝对化或符号链接解析。
std::filesystem::path path_from_utf8(std::string_view text);
std::string path_to_utf8(const std::filesystem::path& path);
/// @brief 输出 generic 格式的路径文本，供 Git 路径语法等调用方使用。
std::string path_to_generic_utf8(const std::filesystem::path& path);

/// @brief 优先 weakly_canonical：解析已存在部分的符号链接，并规范化剩余路径。
/// 若失败，使用 absolute 后做 lexically_normal；若 absolute 也失败，
/// 返回原路径的 lexically_normal，此时结果可能仍是相对路径。
/// 文件系统操作使用 error_code 重载；不捕获内存分配等异常。
std::filesystem::path canonical_or_absolute_or_lexical(const std::filesystem::path& path);

} // namespace dagent::base
