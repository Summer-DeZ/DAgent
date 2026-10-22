/// @file grapheme_tables.hpp
/// @brief 脚本生成的 Unicode 区段表：显示宽度与 UAX #29 字素簇断行属性；查表逻辑在 grapheme.cpp。
#pragma once

#include <cstdint>
#include <span>

namespace dagent::tui::unicode::tables {

struct Range {
    char32_t lo;
    char32_t hi;
};

/// @brief UAX #29 字素簇断行属性（GraphemeBreakProperty.txt）；other 为默认值。
enum class Gcb : uint8_t {
    other = 0,
    cr,
    lf,
    control,
    extend,
    zwj,
    ri,
    prepend,
    spacing_mark,
    l,
    v,
    t,
    lv,
    lvt,
};

struct GcbRange {
    char32_t lo;
    char32_t hi;
    Gcb gc;
};

extern const std::span<const Range> wide;       ///< 宽度 2：EastAsianWidth W/F 与 Emoji_Presentation=Yes
extern const std::span<const Range> joiner;     ///< 宽度 0：连接符
extern const std::span<const Range> zero;       ///< 宽度 0：组合标记等
extern const std::span<const Range> extpict;    ///< Extended_Pictographic（GB11）
extern const std::span<const GcbRange> gcb;     ///< 字素簇断行属性区段

} // namespace dagent::tui::unicode::tables
