/// @file markdown_detail.hpp
/// @brief Markdown 行级判定：流式分块（markdown.cpp）与排版渲染（markdown_render.cpp）共用。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dagent::tui::markdown_detail {

inline bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }
std::string_view drop_cr(std::string_view s) noexcept;
bool blank(std::string_view s) noexcept;
/// @brief 块级标记最多允许 3 个前导空格。
size_t leading_spaces(std::string_view s) noexcept;
bool hr(std::string_view s) noexcept;
bool heading(std::string_view s) noexcept;
/// @brief 列表项标记前允许任意缩进。
bool list_item(std::string_view s) noexcept;
bool quote_line(std::string_view s) noexcept;
bool starts_pipe(std::string_view s) noexcept;
/// @brief 表格分隔行：以 '|' 开头，单元只含 '-' ':' 空白，且至少一个 '-'。
bool table_sep(std::string_view s) noexcept;

enum class LineClass : uint8_t {
    blank,
    hr,
    table_sep,
    heading,
    list,
    quote,
    text,
};

LineClass classify(std::string_view line) noexcept;

} // namespace dagent::tui::markdown_detail
