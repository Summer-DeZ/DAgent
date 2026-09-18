#include "base/text.hpp"

#include <cstdint>
#include <format>
#include <stdexcept>

namespace dagent::base {
namespace {

constexpr std::string_view kReplacement = "\xEF\xBF\xBD"; // U+FFFD

bool is_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

// 返回 s[pos] 处合法 UTF-8 序列的长度；非法（含过长编码、代理区、超范围、截断）返回 0。
std::size_t utf8_sequence_length(std::string_view s, std::size_t pos) {
    const auto lead = static_cast<unsigned char>(s[pos]);
    if (lead < 0x80) return 1;
    const auto continuation_at = [&](std::size_t offset) {
        return pos + offset < s.size() &&
               is_continuation(static_cast<unsigned char>(s[pos + offset]));
    };

    if (lead >= 0xC2 && lead <= 0xDF) return continuation_at(1) ? 2 : 0;
    if (lead >= 0xE0 && lead <= 0xEF) {
        if (!continuation_at(1) || !continuation_at(2)) return 0;
        const auto second = static_cast<unsigned char>(s[pos + 1]);
        if (lead == 0xE0 && second < 0xA0) return 0; // overlong
        if (lead == 0xED && second > 0x9F) return 0; // U+D800–DFFF 代理区
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        if (!continuation_at(1) || !continuation_at(2) || !continuation_at(3)) return 0;
        const auto second = static_cast<unsigned char>(s[pos + 1]);
        if (lead == 0xF0 && second < 0x90) return 0; // overlong
        if (lead == 0xF4 && second > 0x8F) return 0; // 超过 U+10FFFF
        return 4;
    }
    return 0;
}

int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool is_csi_parameter(unsigned char c) {
    return c >= 0x20 && c <= 0x3F; // 中间字节 0x20–0x2F + 参数字节 0x30–0x3F
}

bool is_escape_final(unsigned char c) {
    return c >= 0x30 && c <= 0x7E;
}

// 处理 ESC ] / P / _ / ^ / X 开头的字符串控制序列。OSC 额外接受 BEL 结束，
// 其余只认 ST（ESC \）。遇到非法 C0 控制字符或新的 ESC 序列就中止：中止处
// 的字节保留为普通文本，已扫过的控制串内容丢弃。返回新的扫描位置。
std::size_t skip_string_sequence(std::string_view s, std::size_t pos, bool bel_terminates) {
    for (std::size_t i = pos + 2; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (bel_terminates && c == 0x07) return i + 1;
        if (c == 0x1B) return (i + 1 < s.size() && s[i + 1] == '\\') ? i + 2 : i;
        if (c < 0x20) return i;
    }
    return s.size(); // 末尾被截断：整段丢弃
}

} // namespace

bool is_valid_utf8(std::string_view s) {
    for (std::size_t pos = 0; pos < s.size();) {
        const std::size_t length = utf8_sequence_length(s, pos);
        if (length == 0) return false;
        pos += length;
    }
    return true;
}

std::string to_valid_utf8(std::string_view s, bool* lossy) {
    std::string out;
    out.reserve(s.size());
    bool replaced = false;
    for (std::size_t pos = 0; pos < s.size();) {
        const std::size_t length = utf8_sequence_length(s, pos);
        if (length == 0) {
            out += kReplacement;
            ++pos;
            replaced = true;
        } else {
            out.append(s.substr(pos, length));
            pos += length;
        }
    }
    if (lossy) *lossy = replaced;
    return out;
}

std::size_t utf8_floor(std::string_view s, std::size_t pos) {
    if (pos > s.size()) return s.size();
    while (pos > 0 && pos < s.size() && is_continuation(static_cast<unsigned char>(s[pos]))) --pos;
    return pos;
}

Truncated truncate_middle(std::string_view s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return {std::string(s), s.size(), false};

    const std::size_t head_budget = max_bytes / 2;
    const std::size_t tail_budget = max_bytes - head_budget;
    const std::size_t head_end = utf8_floor(s, head_budget);
    std::size_t tail_start = s.size() - tail_budget;
    while (tail_start < s.size() && is_continuation(static_cast<unsigned char>(s[tail_start])))
        ++tail_start;

    std::string out;
    out.reserve(head_end + 32 + (s.size() - tail_start));
    out.append(s.substr(0, head_end));
    out += std::format("\n…省略 {} 字节…\n", tail_start - head_end);
    out.append(s.substr(tail_start));
    return {std::move(out), s.size(), true};
}

std::string strip_ansi(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    while (pos < s.size()) {
        const auto c = static_cast<unsigned char>(s[pos]);
        if (c != 0x1B) {
            out += static_cast<char>(c);
            ++pos;
            continue;
        }
        if (pos + 1 >= s.size()) break; // 末尾半个 ESC：丢弃
        const auto next = static_cast<unsigned char>(s[pos + 1]);

        if (next == '[') { // CSI：参数字节 0x20–0x3F，最终字节 0x40–0x7E
            std::size_t end = pos + 2;
            while (end < s.size() && is_csi_parameter(static_cast<unsigned char>(s[end]))) ++end;
            if (end < s.size() && static_cast<unsigned char>(s[end]) >= 0x40 &&
                static_cast<unsigned char>(s[end]) <= 0x7E) {
                pos = end + 1; // 合法序列
            } else if (end >= s.size()) {
                pos = s.size(); // 末尾被截断：整段丢弃
            } else {
                pos += 2; // 非法：只丢 ESC [，后面的字节是正文，保留
            }
        } else if (next == ']' || next == 'P' || next == '_' || next == '^' || next == 'X') {
            pos = skip_string_sequence(s, pos, next == ']');
        } else { // ESC + 中间字节 0x20–0x2F + 最终字节 0x30–0x7E，如 ESC ( B、ESC # 8
            std::size_t end = pos + 1;
            while (end < s.size() && static_cast<unsigned char>(s[end]) >= 0x20 &&
                   static_cast<unsigned char>(s[end]) <= 0x2F)
                ++end;
            if (end < s.size() && is_escape_final(static_cast<unsigned char>(s[end]))) {
                pos = end + 1; // 合法序列
            } else if (end >= s.size()) {
                pos = s.size(); // 末尾被截断：整段丢弃
            } else {
                pos += 1; // 孤立的 ESC：只丢 ESC
            }
        }
    }
    return out;
}

std::string base64_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size() / 4 * 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char raw : s) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '=') break;
        if (c == '\r' || c == '\n') continue;
        const int value = base64_value(c);
        if (value < 0) throw std::invalid_argument("invalid base64 character");
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((accumulator >> bits) & 0xFF);
        }
    }
    return out;
}

} // namespace dagent::base
