/// @file grapheme.hpp
/// @brief UTF-8 解码、字素簇聚合、码点显示宽度。
#pragma once

#include <cstddef>
#include <string_view>

namespace dagent::tui::unicode {

/// @brief 码点显示宽度：0（组合记号/格式/控制）、1（半角）、2（全角/emoji）。
int char_width(char32_t cp) noexcept;

/// @brief 解码一个码点并前移视图；非法字节消费 1 字节并返回 U+FFFD。
char32_t decode_utf8(std::string_view& s) noexcept;

/// @brief 编码一个码点，返回字节数（1-4）；非法码点编码为单个空格。
std::size_t encode_utf8(char32_t cp, char (&out)[4]) noexcept;

/// @brief 一个字素簇及其显示宽度。
struct Grapheme {
    std::string_view bytes; ///< UTF-8 字节切片（指向调用方缓冲）
    int width = 0;          ///< 显示宽度
};

/// @brief 取下一个字素簇并前移视图；末尾返回 false。
bool next_grapheme(std::string_view& s, Grapheme& out) noexcept;

} // namespace dagent::tui::unicode
