// L5 折行核心与默认块渲染器（文档§8.2/§8.5/§8.6）。
// 计数与物化走同一条扫描路径，从根本上保证 §十三.1 的等价性：
//   count_rows(s, w) == 物化出的行数（同一字素切分、同一 tab 展开、
//   同一断点决策）。
#include "tui/document.hpp"

#include "tui/grapheme.hpp"

#include <algorithm>

namespace dagent::tui {

namespace {

constexpr int k_tab_stop = 8;

// 与 Surface::text 完全一致的列推进：制表符展开到 tab stop（行内相对
// 起点），其余控制符零宽。'\n'/'\r' 由 wrap_row 先行处理，不会到这里。
int advance(int col, const unicode::Grapheme& g) noexcept {
    if (g.bytes.size() == 1) {
        const unsigned char b = static_cast<unsigned char>(g.bytes[0]);
        if (b == '\t') return (col / k_tab_stop + 1) * k_tab_stop;
        if (b < 0x20 || b == 0x7F) return col;
    }
    return col + g.width;
}

struct RowEnd {
    size_t end = 0;  // 本行内容为 [begin, end)
    size_t next = 0; // 下一行起始字节（换行结束则跨过换行符）
    int width = 0;   // 本行显示宽度
};

// 扫一行：宽度到顶就回退到最后一个空格断点（没有则按字素硬断，
// 长单词/URL 不丢内容），换行符结束本行。
RowEnd wrap_row(std::string_view s, size_t from, int width) noexcept {
    int col = 0;
    int brk_col = 0;
    size_t pos = from;
    size_t brk = from; // 断点候选：空格之后
    while (pos < s.size()) {
        std::string_view rest = s.substr(pos);
        unicode::Grapheme g;
        if (!unicode::next_grapheme(rest, g)) break;
        const size_t next = pos + g.bytes.size();
        const unsigned char b = static_cast<unsigned char>(s[pos]);
        if (b == '\n') return {pos, next, col};
        if (b == '\r') {
            return {pos, (next < s.size() && s[next] == '\n') ? next + 1 : next,
                    col};
        }
        const int nc = advance(col, g);
        if (nc > width && col > 0) {
            if (brk > from) return {brk, brk, brk_col};
            return {pos, pos, col};
        }
        // col == 0 时即便单簇超宽也整簇消费，保证扫描必然前进
        // （宽字符比视图还宽是病态输入，计数与物化保持一致即可）。
        col = nc;
        if (b == ' ') {
            brk = next;
            brk_col = col;
        }
        pos = next;
    }
    return {pos, pos, col};
}

WrapResult measure_from(std::string_view s, size_t from, int width) noexcept {
    WrapResult r{};
    r.stable_bytes = from; // 不稳定尾行的起始位置；包装层转成相对偏移
    size_t pos = from;
    while (pos < s.size()) {
        const RowEnd row = wrap_row(s, pos, width);
        ++r.rows;
        pos = row.next;
        if (row.next > row.end) {
            ++r.stable_rows; // 换行结束：断点由已有字符唯一决定
            r.stable_bytes = row.next;
        } else if (row.next < s.size()) {
            ++r.stable_rows; // 超宽硬断：同上
            r.stable_bytes = row.end;
        }
        // 其余情况：输入末尾的不完整行，追加内容可能改变它，不稳定
    }
    return r;
}

// 把 [begin, end) 拷成可绘制文本：制表符展开为到下一 stop 的空格
// （§5.4），其余字节原样。控制符保留但宽度贡献 0，Surface::text 会跳过。
void append_row_text(std::string& dst, std::string_view src, size_t begin,
                     size_t end) {
    dst.clear(); // 保留容量：增量物化原地复用行对象，稳态零分配
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

// pos 所在逻辑行（'\n'/'\r' 分隔）的起始字节。UTF-8 续字节不可能是
// 0x0A/0x0D，逐字节回扫安全；只在增量折行跨逻辑行时用到，代价 O(行内偏移)。
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
    append_row_text(ln.spans[0].text, src, begin, end);
    ln.spans[0].style = st;
    ln.width = width;
}

// 通用物化：逐逻辑行折行，样式由 style_of(逻辑行) 决定。
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
            const RowEnd row = wrap_row(src, pos, width);
            put_row(out, n, src, pos, row.end, row.width, st);
            ++n;
            const bool crossed = row.next > row.end;
            pos = row.next;
            if (crossed || pos >= src.size() || n >= limit) break;
        }
    }
    return n;
}

} // namespace

WrapResult wrap_measure_from(std::string_view source, size_t from,
                             int width) noexcept {
    if (from >= source.size()) return {};
    WrapResult r = measure_from(source, from, width);
    r.stable_bytes -= from; // 契约：相对扫描起点
    return r;
}

WrapResult wrap_measure(std::string_view source, int width) noexcept {
    return wrap_measure_from(source, 0, width);
}

size_t count_rows(std::string_view source, int width) noexcept {
    return wrap_measure(source, width).rows;
}

// ---- 默认渲染器 ----

size_t TextRenderer::count(std::string_view source, int width) const {
    return wrap_measure(source, width).rows;
}

size_t TextRenderer::render(const Block& block, int width, const Theme& theme,
                            size_t from, size_t valid,
                            std::vector<Line>& out) const {
    return materialize_rows(block, width, from, valid, out,
                            [this, &theme](std::string_view, size_t) {
                                return theme.*slot_;
                            });
}

size_t DiffRenderer::count(std::string_view source, int width) const {
    return wrap_measure(source, width).rows;
}

size_t DiffRenderer::render(const Block& block, int width, const Theme& theme,
                            size_t from, size_t valid,
                            std::vector<Line>& out) const {
    return materialize_rows(
        block, width, from, valid, out,
        [&theme](std::string_view src, size_t pos) {
            switch (src[line_start_of(src, pos)]) {
            case '+':
                return theme.add;
            case '-':
                return theme.del;
            case '@':
                return theme.dim;
            default:
                return theme.text;
            }
        });
}

} // namespace dagent::tui
