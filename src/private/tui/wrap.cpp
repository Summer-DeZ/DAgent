#include "tui/document.hpp"

#include "tui/grapheme.hpp"

#include <algorithm>

namespace dagent::tui {

namespace {

constexpr int k_tab_stop = 8;

/// @brief 与 Surface::text 一致的列推进：tab 展开，其余控制符零宽。
int advance(int col, const unicode::Grapheme& g) noexcept {
    if (g.bytes.size() == 1) {
        const unsigned char b = static_cast<unsigned char>(g.bytes[0]);
        if (b == '\t') return (col / k_tab_stop + 1) * k_tab_stop;
        if (b < 0x20 || b == 0x7F) return col;
    }
    return col + g.width;
}

/// @brief 去掉末尾不完整 UTF-8 序列后的长度（流式分块可能从多字节字符中间切开）。
size_t complete_prefix(std::string_view s) noexcept {
    const size_t n = s.size();
    for (size_t k = 1; k <= 3 && k <= n; ++k) {
        const unsigned char b = static_cast<unsigned char>(s[n - k]);
        if ((b & 0xC0) == 0x80) continue;
        const size_t len = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        return len > k ? n - k : n;
    }
    return n;
}

WrapResult measure_from(std::string_view s, size_t from, int width) noexcept {
    WrapResult r{};
    r.stable_bytes = from; // 不稳定尾行的起始位置
    const size_t complete = complete_prefix(s);
    size_t pos = from;
    while (pos < s.size()) {
        const RowEdge row = wrap_next_row(s, pos, width);
        ++r.rows;
        pos = row.next;
        const bool newline = row.next > row.end;
        // 行尾孤立 '\r'：后续 '\n' 可能并入，断点未定。
        const bool cr_at_end = newline && s[row.end] == '\r' && row.next == s.size();
        if (newline && !cr_at_end) {
            ++r.stable_rows; // 换行结束：断点已定
            r.stable_bytes = row.next;
        } else if (!newline && row.next < s.size() && row.cut < complete) {
            // 超宽断行：断点已定；行尾不完整序列不算。
            ++r.stable_rows;
            r.stable_bytes = row.end;
        }
    }
    return r;
}

/// @brief pos 所在逻辑行（'\n'/'\r' 分隔）的起始字节。
size_t line_start_of(std::string_view s, size_t pos) noexcept {
    size_t i = pos;
    while (i > 0 && s[i - 1] != '\n' && s[i - 1] != '\r') --i;
    return i;
}

void put_row(std::vector<Line>& out, size_t i, std::string_view src, size_t begin,
             size_t end, int width, const Style& st) {
    if (out.size() <= i) out.resize(i + 1);
    Line& ln = out[i];
    ln.spans.resize(1); // 复用首个 span 的字符串容量
    expand_row(ln.spans[0].text, src, begin, end);
    ln.spans[0].style = st;
    ln.spans[0].src = begin;
    ln.width = width;
    ln.offset = begin;
    ln.lex = 0;
}

/// @brief 通用物化：逐逻辑行折行，样式由 style_of(逻辑行) 决定。
template <class StyleOf>
size_t materialize_rows(const Block& b, int width, size_t from, size_t valid,
                        std::vector<Line>& out, StyleOf&& style_of) {
    const std::string_view src = b.source;
    const size_t limit = b.collapsed
                             ? std::min<size_t>(b.collapsed_rows, b.row_count)
                             : b.row_count;
    size_t n = valid;
    size_t pos = from;
    while (pos < src.size() && n < limit) {
        const Style st = style_of(src, pos);
        for (;;) {
            const RowEdge row = wrap_next_row(src, pos, width);
            put_row(out, n, src, pos, row.end, row.width, st);
            ++n;
            const bool crossed = row.next > row.end;
            pos = row.next;
            if (crossed || pos >= src.size() || n >= limit) break;
        }
    }
    return n;
}

/// @brief 断行禁则：闭合标点不出现在行首。
bool no_line_start(std::string_view g) noexcept {
    static constexpr std::string_view k_closing[] = {
        "，", "。", "、", "；", "：", "！", "？", "）", "」", "』", "】", "》",
        "〉", "’", "”", "…", "～", "·", "％", "．", "］", "｝", "〕",
        ",",  ".",  ";",  ":",  "!",  "?",  ")",  "]",  "}",  "%",
    };
    for (std::string_view c : k_closing) {
        if (g == c) return true;
    }
    return false;
}

/// @brief 开启标点不出现在行尾。
bool no_line_end(std::string_view g) noexcept {
    static constexpr std::string_view k_opening[] = {
        "（", "「", "『", "【", "《", "〈", "‘", "“", "［", "｛", "〔",
        "(",  "[",  "{",
    };
    for (std::string_view c : k_opening) {
        if (g == c) return true;
    }
    return false;
}

} // namespace

// 断点候选：空格之后或宽字符前后（受禁则约束）；无断点按字素硬断。
RowEdge wrap_next_row(std::string_view s, size_t from, int width) noexcept {
    int col = 0;
    int brk_col = 0;
    size_t pos = from;
    size_t brk = from; // 断点候选：下一行的起点
    std::string_view prev;  // 上一个字素
    int prev_width = 0;
    while (pos < s.size()) {
        std::string_view rest = s.substr(pos);
        unicode::Grapheme g;
        if (!unicode::next_grapheme(rest, g)) break;
        const size_t next = pos + g.bytes.size();
        const unsigned char b = static_cast<unsigned char>(s[pos]);
        if (b == '\n') return {pos, next, col, pos};
        if (b == '\r') {
            return {pos, (next < s.size() && s[next] == '\n') ? next + 1 : next,
                    col, pos};
        }
        // 宽字符前后登记断点候选。
        if (pos > from && (g.width == 2 || prev_width == 2) &&
            !no_line_start(g.bytes) && !no_line_end(prev)) {
            brk = pos;
            brk_col = col;
        }
        const int nc = advance(col, g);
        if (nc > width && col > 0) {
            if (brk > from) return {brk, brk, brk_col, pos};
            return {pos, pos, col, pos};
        }
        // col == 0：整簇消费，保证扫描前进。
        col = nc;
        if (b == ' ') {
            brk = next;
            brk_col = col;
        }
        prev = g.bytes;
        prev_width = g.width;
        pos = next;
    }
    return {pos, pos, col, pos};
}

void expand_row(std::string& dst, std::string_view src, size_t begin,
                size_t end) {
    dst.clear(); // 保留容量
    std::string_view row = src.substr(begin, end - begin);
    int col = 0;
    while (!row.empty()) {
        unicode::Grapheme g;
        if (!unicode::next_grapheme(row, g)) break;
        if (g.bytes.size() == 1 && g.bytes[0] == '\t') {
            const int stop = (col / k_tab_stop + 1) * k_tab_stop;
            dst.append(static_cast<size_t>(stop - col), ' ');
            col = stop;
            continue;
        }
        dst += g.bytes;
        col += g.width;
    }
}

WrapResult wrap_measure_from(std::string_view source, size_t from,
                             int width) noexcept {
    if (from >= source.size()) return {};
    WrapResult r = measure_from(source, from, width);
    r.stable_bytes -= from; // 转为相对 from
    return r;
}

size_t count_rows(std::string_view source, int width) noexcept {
    return wrap_measure_from(source, 0, width).rows;
}

// ---- 默认渲染器 ----

size_t TextRenderer::render(const Block& block, int width, const ThemeTokens& theme,
                            size_t from, size_t valid,
                            std::vector<Line>& out) const {
    return materialize_rows(block, width, from, valid, out,
                            [this, &theme](std::string_view, size_t) {
                                return theme.*slot_;
                            });
}

size_t DiffRenderer::render(const Block& block, int width,
                            const ThemeTokens& theme, size_t from, size_t valid,
                            std::vector<Line>& out) const {
    return materialize_rows(
        block, width, from, valid, out,
        [&theme](std::string_view src, size_t pos) {
            switch (src[line_start_of(src, pos)]) {
            case '+':
                return theme.diff_added;
            case '-':
                return theme.diff_removed;
            case '@':
                return theme.diff_hunk;
            default:
                return theme.text;
            }
        });
}

} // namespace dagent::tui
