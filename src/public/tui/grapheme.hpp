// L2 内部依赖：UTF-8 解码、字素簇聚合、码点显示宽度（§7）。
// 独立成编译单元是因为它是每帧热路径，且区间表体量大；宽度与断行属性表
// 由 Unicode 数据文件生成（见 grapheme.cpp 生成区段，勿手改）。
// 字素簇按 UAX #29 聚合，有意不实现 GB9b（Prepend）与 GB9c（Indic 连缀）。
#pragma once

#include <cstddef>
#include <string_view>

namespace dagent::tui::unicode {

// 码点显示宽度：0（组合记号/格式/控制）、1（半角）、2（全角/emoji）。
// 不依赖 locale 的 wcwidth；BMP 直接数组索引 memoize，星面区间二分。
int char_width(char32_t cp) noexcept;

// 解码一个码点并前移视图。合法消费 1-4 字节；
// 非法字节消费 1 字节并返回 U+FFFD；空视图返回 0（调用方先判空）。
char32_t decode_utf8(std::string_view& s) noexcept;

// 编码一个码点，返回字节数（1-4）；非法码点编码为单个空格。
std::size_t encode_utf8(char32_t cp, char (&out)[4]) noexcept;

struct Grapheme {
    std::string_view bytes; // UTF-8 字节切片（指向调用方缓冲）
    int width = 0;
};

// 取下一个字素簇并前移视图；末尾返回 false。
// 覆盖 UAX #29 主要规则：组合记号、VS15/VS16、ZWJ 序列、区域指示符成对。
bool next_grapheme(std::string_view& s, Grapheme& out) noexcept;

} // namespace dagent::tui::unicode
