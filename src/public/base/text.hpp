/// @file text.hpp
/// @brief 文本工具：UTF-8 校验与修复、按字符边界截断、ANSI 转义清理、base64 解码。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace dagent::base {

/// @brief 严格校验：拒绝过长编码（overlong）、U+D800–DFFF 代理区、超过 U+10FFFF 的码点。
bool is_valid_utf8(std::string_view s);

/// @brief 把非法字节替换成 U+FFFD，返回一定合法的 UTF-8；lossy 记录是否发生过替换。
std::string to_valid_utf8(std::string_view s, bool* lossy = nullptr);

/// @brief 从 pos 往前退到 UTF-8 字符边界（pos 在字符中间时退到该字符起始处）。pos 超过长度时返回长度。
std::size_t utf8_floor(std::string_view s, std::size_t pos);

/// @brief 保留头部和尾部，中间换成 "\n…省略 N 字节…\n"；两处切点都落在 UTF-8 字符边界上。
/// 头尾各分一半预算，标记本身不计入预算；未超限时原样返回。
std::string truncate_middle(std::string_view s, std::size_t max_bytes);

/// @brief 去掉 CSI、OSC/DCS/APC/PM/SOS（OSC 到 BEL 或 ST，其余到 ST）、带中间字节或
/// 单字符的 ESC 序列。非法或不完整的序列只丢引导部分，后面的字节按正文保留；
/// 末尾被截断的序列整段丢弃。
std::string strip_ansi(std::string_view s);

/// @brief 解码标准 base64（允许 CR/LF 换行）；出现非字母表字符抛 std::invalid_argument。
std::string base64_decode(std::string_view s);

} // namespace dagent::base
