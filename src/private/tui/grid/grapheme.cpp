#include "tui/grid/grapheme.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

#include "tui/grid/grapheme_tables.hpp"

namespace dagent::tui::unicode {

namespace {

using tables::Gcb;
using tables::GcbRange;
using tables::Range;

constexpr uint8_t k_extpict_bit = 0x80; ///< BMP 表的最高位标记 Extended_Pictographic（GB11）

/// @brief 区间表按 lo 升序存放，二分查找。
bool in_sorted(std::span<const Range> a, char32_t cp) noexcept {
    std::size_t lo = 0, hi = a.size();
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

Gcb gcb_of_range(char32_t cp) noexcept {
    std::size_t lo = 0;
    std::size_t hi = tables::gcb.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (cp < tables::gcb[mid].lo) {
            hi = mid;
        } else if (cp > tables::gcb[mid].hi) {
            lo = mid + 1;
        } else {
            return tables::gcb[mid].gc;
        }
    }
    return Gcb::other;
}

void mark_range(std::array<uint8_t, 0x10000>& t, const Range& r,
                uint8_t value) noexcept {
    const auto lo = static_cast<std::size_t>(r.lo);
    const auto hi = static_cast<std::size_t>(std::min<char32_t>(r.hi, 0xFFFF));
    for (std::size_t cp = lo; cp <= hi; ++cp) {
        t[cp] = value;
    }
}

/// @brief 宽度表：BMP 一次构建（64KB），星面走区间二分。
const std::array<uint8_t, 0x10000>& bmp_width() noexcept {
    static const std::array<uint8_t, 0x10000> table = [] {
        std::array<uint8_t, 0x10000> t{};
        t.fill(1);
        // wide 最后写：与 joiner/zero 的区间重叠处以宽为准。
        for (const Range& r : tables::zero) mark_range(t, r, 0);
        for (const Range& r : tables::joiner) mark_range(t, r, 0);
        for (const Range& r : tables::wide) mark_range(t, r, 2);
        return t;
    }();
    return table;
}

/// @brief GCB 表：低 4 位类别，最高位 Extended_Pictographic。
const std::array<uint8_t, 0x10000>& bmp_gcb() noexcept {
    static const std::array<uint8_t, 0x10000> table = [] {
        std::array<uint8_t, 0x10000> t{};
        for (const GcbRange& r : tables::gcb) {
            const auto lo = static_cast<std::size_t>(r.lo);
            const auto hi =
                static_cast<std::size_t>(std::min<char32_t>(r.hi, 0xFFFF));
            for (std::size_t cp = lo; cp <= hi; ++cp) {
                t[cp] = static_cast<uint8_t>(r.gc);
            }
        }
        for (const Range& r : tables::extpict) {
            const auto lo = static_cast<std::size_t>(r.lo);
            const auto hi =
                static_cast<std::size_t>(std::min<char32_t>(r.hi, 0xFFFF));
            for (std::size_t cp = lo; cp <= hi; ++cp) t[cp] |= k_extpict_bit;
        }
        return t;
    }();
    return table;
}

Gcb gcb_of(char32_t cp) noexcept {
    if (cp < 0x10000) return static_cast<Gcb>(bmp_gcb()[cp] & 0x0F);
    return gcb_of_range(cp);
}

bool is_extpict(char32_t cp) noexcept {
    if (cp < 0x10000) return (bmp_gcb()[cp] & k_extpict_bit) != 0;
    return in_sorted(tables::extpict, cp);
}

} // namespace

int char_width(char32_t cp) noexcept {
    if (cp < 0x10000) return bmp_width()[cp];
    if (in_sorted(tables::zero, cp) || in_sorted(tables::joiner, cp)) return 0;
    if (in_sorted(tables::wide, cp)) return 2;
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
    for (std::size_t i = 1; i < len; ++i) {
        // 末尾截断：续字节整体作废；非续字节只丢首字节，其后合法内容保留。
        if (i >= s.size()) {
            s.remove_prefix(s.size());
            return 0xFFFD;
        }
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

/// 按 UAX #29 实现；GB9b/GB9c 有意不实现。簇宽度取簇内码点最大宽度。
bool next_grapheme(std::string_view& s, Grapheme& out) noexcept {
    if (s.empty()) {
        return false;
    }
    std::string_view cursor = s;
    const char32_t first = decode_utf8(cursor);
    Gcb prev = gcb_of(first);
    int width = char_width(first);
    bool vs16 = false;
    // GB11 状态：已消费的前缀匹配 ExtPict (Extend|ZWJ)*。
    bool gb11_ready = is_extpict(first);
    int ri_count = prev == Gcb::ri ? 1 : 0;

    while (!cursor.empty()) {
        std::string_view probe = cursor;
        const char32_t next = decode_utf8(probe);
        const Gcb nclass = gcb_of(next);
        bool brk;
        if (prev == Gcb::cr && nclass == Gcb::lf) {
            brk = false; // GB3
        } else if (prev == Gcb::control || prev == Gcb::cr ||
                   prev == Gcb::lf) {
            brk = true; // GB4
        } else if (nclass == Gcb::control || nclass == Gcb::cr ||
                   nclass == Gcb::lf) {
            brk = true; // GB5
        } else if (prev == Gcb::l &&
                   (nclass == Gcb::l || nclass == Gcb::v ||
                    nclass == Gcb::lv || nclass == Gcb::lvt)) {
            brk = false; // GB6
        } else if ((prev == Gcb::lv || prev == Gcb::v) &&
                   (nclass == Gcb::v || nclass == Gcb::t)) {
            brk = false; // GB7
        } else if ((prev == Gcb::lvt || prev == Gcb::t) &&
                   nclass == Gcb::t) {
            brk = false; // GB8
        } else if (nclass == Gcb::extend || nclass == Gcb::zwj) {
            brk = false; // GB9
        } else if (nclass == Gcb::spacing_mark) {
            brk = false; // GB9a
        } else if (prev == Gcb::zwj && gb11_ready && is_extpict(next)) {
            brk = false; // GB11
        } else if (prev == Gcb::ri && nclass == Gcb::ri &&
                   (ri_count & 1) == 1) {
            brk = false; // GB12/GB13
        } else {
            brk = true; // GB999
        }
        if (brk) {
            break;
        }
        cursor = probe;
        if (next == 0xFE0F) vs16 = true; // VS16
        width = std::max(width, char_width(next));
        if (is_extpict(next)) {
            gb11_ready = true;
        } else if (nclass != Gcb::extend && nclass != Gcb::zwj) {
            gb11_ready = false;
        }
        if (nclass == Gcb::ri) ++ri_count;
        prev = nclass;
    }

    if (vs16) width = std::max(width, 2);      // 后随 VS16 的簇 2 列
    if (ri_count >= 2) width = std::max(width, 2); // 国旗成对占 2 列
    out.bytes = s.substr(0, s.size() - cursor.size());
    out.width = width;
    s = cursor;
    return true;
}

} // namespace dagent::tui::unicode
