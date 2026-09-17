#include "tui/grapheme.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace dagent::tui::unicode {

namespace {

struct Range {
    char32_t lo;
    char32_t hi;
};

// EastAsianWidth W/F 主要区间的手工覆盖（源自 EastAsianWidth.txt；
// 体量可控、覆盖 CJK/全角/常用 emoji；后续可由生成脚本整体替换）。
constexpr Range kWide[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
    {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
    {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
    {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE},
    {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
    {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
    {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
    {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
    {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF},
    {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF},
    {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6},
    {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F200, 0x1F320}, {0x1F32D, 0x1F335},
    {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA},
    {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4},
    {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC},
    {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567},
    {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4},
    {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC},
    {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7}, {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF},
    {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

// 簇内 joiner：组合记号（Mn/Me）、变体选择符、emoji 肤色修饰。
// 与 kZero 的区别：这些粘附到前一簇，kZero 自成零宽簇。
constexpr Range kJoiner[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A},
    {0x07A6, 0x07B0}, {0x07EB, 0x07F3}, {0x0816, 0x0819}, {0x081B, 0x0823},
    {0x0825, 0x0827}, {0x0829, 0x082D}, {0x0859, 0x085B}, {0x08D3, 0x08E1},
    {0x093C, 0x093C}, {0x0951, 0x0957}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A},
    {0x0E47, 0x0E4E}, {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EBC}, {0x0EC8, 0x0ECD},
    {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20F0},
    {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0x1F3FB, 0x1F3FF},
    {0xE0100, 0xE01EF},
};

// 自成零宽簇：控制符与格式字符（Cf/Cc），不留格。
constexpr Range kZero[] = {
    {0x0000, 0x001F}, {0x007F, 0x009F},
    {0x200B, 0x200F}, {0x2028, 0x202E}, {0x2060, 0x2064}, {0xFEFF, 0xFEFF},
};

// 区间表按 lo 升序存放，二分查找。
bool in_sorted(const Range* a, std::size_t n, char32_t cp) noexcept {
    std::size_t lo = 0, hi = n;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (cp < a[mid].lo) {
            hi = mid;
        } else if (cp > a[mid].hi) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

template <std::size_t N>
bool in_sorted(const Range (&a)[N], char32_t cp) noexcept {
    return in_sorted(a, N, cp);
}

constexpr bool is_regional_indicator(char32_t cp) noexcept {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

void mark_range(std::array<uint8_t, 0x10000>& t, const Range& r,
                uint8_t value) noexcept {
    const auto lo = static_cast<std::size_t>(r.lo);
    const auto hi = static_cast<std::size_t>(std::min<char32_t>(r.hi, 0xFFFF));
    for (std::size_t cp = lo; cp <= hi; ++cp) {
        t[cp] = value;
    }
}

std::array<uint8_t, 0x10000> build_bmp_table() noexcept {
    std::array<uint8_t, 0x10000> t{};
    t.fill(1);
    // wide 最后写：与 joiner/zero 的区间重叠处以宽为准
    for (const Range& r : kZero) mark_range(t, r, 0);
    for (const Range& r : kJoiner) mark_range(t, r, 0);
    for (const Range& r : kWide) mark_range(t, r, 2);
    return t;
}

} // namespace

int char_width(char32_t cp) noexcept {
    // BMP 直接索引（64KB，进程一次构建）；静态局部初始化后为纯数组读取。
    static const std::array<uint8_t, 0x10000> kTable = build_bmp_table();
    if (cp < 0x10000) {
        return kTable[static_cast<std::size_t>(cp)];
    }
    if (in_sorted(kJoiner, cp)) {
        return 0;
    }
    if (in_sorted(kWide, cp)) {
        return 2;
    }
    return 1;
}

char32_t decode_utf8(std::string_view& s) noexcept {
    const unsigned char b0 = static_cast<unsigned char>(s[0]);
    std::size_t len = 0;
    char32_t cp = 0;
    if (b0 < 0x80) {
        len = 1;
        cp = b0;
    } else if ((b0 & 0xE0) == 0xC0) {
        len = 2;
        cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3;
        cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4;
        cp = b0 & 0x07;
    } else {
        s.remove_prefix(1);
        return 0xFFFD;
    }
    if (s.size() < len) {
        s.remove_prefix(s.size());
        return 0xFFFD;
    }
    for (std::size_t i = 1; i < len; ++i) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        if ((b & 0xC0) != 0x80) {
            s.remove_prefix(1);
            return 0xFFFD;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    // 过长编码、代理区、越界一律按非法处理。
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) ||
        cp > 0x10FFFF) {
        s.remove_prefix(1);
        return 0xFFFD;
    }
    s.remove_prefix(len);
    return cp;
}

std::size_t encode_utf8(char32_t cp, char (&out)[4]) noexcept {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = static_cast<char>(0xF0 | (cp >> 18));
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F));
        return 4;
    }
    out[0] = ' ';
    return 1;
}

bool next_grapheme(std::string_view& s, Grapheme& out) noexcept {
    if (s.empty()) {
        return false;
    }
    std::string_view rest = s;
    const char32_t base = decode_utf8(rest);
    int width = char_width(base);
    bool vs16 = false;
    bool ri_pending = is_regional_indicator(base);

    for (;;) {
        if (rest.empty()) {
            break;
        }
        const std::string_view before = rest;
        const char32_t cp = decode_utf8(rest);
        // 国旗：成对区域指示符合成一个 2 列簇。
        if (ri_pending && is_regional_indicator(cp)) {
            ri_pending = false;
            width = 2;
            continue;
        }
        // ZWJ：吸收下一子簇（emoji 家族序列），宽度取各子簇最大值。
        if (cp == 0x200D) {
            if (rest.empty()) {
                break;
            }
            width = std::max(width, char_width(decode_utf8(rest)));
            continue;
        }
        if (cp == 0xFE0F) { // VS16：文本呈现 → emoji 呈现
            vs16 = true;
            continue;
        }
        if (in_sorted(kJoiner, cp)) {
            continue;
        }
        rest = before; // 非成员码点：归还，下一簇从它开始
        break;
    }

    // VS16 提宽只对符号区生效，避免 ASCII 字母被拉成全角。
    if (vs16 && width < 2 && base >= 0x2000) {
        width = 2;
    }
    out.bytes = s.substr(0, s.size() - rest.size());
    out.width = width;
    s = rest;
    return true;
}

} // namespace dagent::tui::unicode
