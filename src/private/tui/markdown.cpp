// L5 流式 Markdown 切分与块渲染（§3.7.2）。
//
// 切分器把一条持续增长的 Markdown 消息切成多个 Document 块，只有最后一个
// 块在增长：段落/标题/列表/引用进 markdown 块，围栏代码进 code 块（info
// 存 meta），表格进 table 块，分隔线自成一块立即关闭。结构判定全部发生在
// 完整行上，因此结果与喂入分块无关；未完成行先追加到当前块立即显示，整行
// 到达后判定为"新结构"时用 replace 回退该行。replace 整块替换，代价是
// O(当前块)，与 markdown/表格块每帧的整块重排同阶。
//
// 渲染：markdown 块隐藏行内标记，按段解析、按显示文本折行（见"Markdown
// 排版"）；code 块由 syntax.cpp 的 SyntaxRenderer 着色；表格整块重排。
#include "tui/document.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/grapheme.hpp"

namespace dagent::tui {

namespace {

constexpr size_t k_npos = static_cast<size_t>(-1);

bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }

std::string_view drop_cr(std::string_view s) noexcept {
    if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
    return s;
}

bool blank(std::string_view s) noexcept {
    for (char c : s) {
        if (!is_space(c)) return false;
    }
    return true;
}

// 块级标记最多允许 3 个前导空格。
size_t leading_spaces(std::string_view s) noexcept {
    size_t i = 0;
    while (i < s.size() && i < 3 && s[i] == ' ') ++i;
    return i;
}

bool fence_open(std::string_view s, char& ch, size_t& len, std::string& info) {
    const size_t i = leading_spaces(s);
    if (i >= s.size()) return false;
    ch = s[i];
    if (ch != '`' && ch != '~') return false;
    size_t j = i;
    while (j < s.size() && s[j] == ch) ++j;
    len = j - i;
    if (len < 3) return false;
    std::string_view rest = s.substr(j);
    if (ch == '`' && rest.find('`') != std::string_view::npos) return false;
    size_t a = 0;
    size_t b = rest.size();
    while (a < b && is_space(rest[a])) ++a;
    while (b > a && is_space(rest[b - 1])) --b;
    info.assign(rest.substr(a, b - a));
    return true;
}

// 闭合围栏：同字符、长度不短于开启、其后只有空白。
bool fence_close(std::string_view s, char ch, size_t len) noexcept {
    const size_t i = leading_spaces(s);
    size_t j = i;
    while (j < s.size() && s[j] == ch) ++j;
    if (j - i < len) return false;
    for (; j < s.size(); ++j) {
        if (!is_space(s[j])) return false;
    }
    return true;
}

// 代码块中未完成行仍可能是闭合围栏（只含围栏字符与空白）。
bool could_close_fence(std::string_view s, char ch) noexcept {
    const size_t i = leading_spaces(s);
    for (size_t j = i; j < s.size(); ++j) {
        if (s[j] == ch || is_space(s[j])) continue;
        return false;
    }
    return true;
}

bool hr(std::string_view s) noexcept {
    const size_t i0 = leading_spaces(s);
    if (i0 >= s.size()) return false;
    const char c = s[i0];
    if (c != '-' && c != '*' && c != '_') return false;
    int count = 0;
    for (size_t i = i0; i < s.size(); ++i) {
        if (s[i] == c) {
            ++count;
        } else if (!is_space(s[i])) {
            return false;
        }
    }
    return count >= 3;
}

bool heading(std::string_view s) noexcept {
    const size_t i = leading_spaces(s);
    if (i >= s.size() || s[i] != '#') return false;
    size_t j = i;
    while (j < s.size() && s[j] == '#') ++j;
    if (j - i > 6) return false;
    return j == s.size() || s[j] == ' ';
}

// 列表项允许任意缩进（嵌套列表常用 2 或 4 个空格；本框架不支持缩进
// 代码块，不存在歧义）。
bool list_item(std::string_view s) noexcept {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    if (i >= s.size()) return false;
    const char c = s[i];
    if (c == '-' || c == '*' || c == '+') {
        return i + 1 == s.size() || is_space(s[i + 1]);
    }
    if (c >= '0' && c <= '9') {
        size_t j = i;
        while (j < s.size() && j - i < 9 && s[j] >= '0' && s[j] <= '9') ++j;
        if (j < s.size() && (s[j] == '.' || s[j] == ')')) {
            return j + 1 == s.size() || is_space(s[j + 1]);
        }
    }
    return false;
}

bool quote_line(std::string_view s) noexcept {
    const size_t i = leading_spaces(s);
    return i < s.size() && s[i] == '>';
}

bool starts_pipe(std::string_view s) noexcept {
    const size_t i = leading_spaces(s);
    return i < s.size() && s[i] == '|';
}

// 表格分隔行：以 '|' 开头，单元只含 '-' ':' 空白，且至少一个 '-'。
bool table_sep(std::string_view s) noexcept {
    if (!starts_pipe(s)) return false;
    bool dash = false;
    for (char c : s) {
        if (c == '|') continue;
        if (c == '-') {
            dash = true;
        } else if (c != ':' && !is_space(c)) {
            return false;
        }
    }
    return dash;
}

enum class LineClass : uint8_t {
    blank,
    hr,
    table_sep,
    heading,
    list,
    quote,
    text,
};

LineClass classify(std::string_view line) noexcept {
    if (blank(line)) return LineClass::blank;
    if (hr(line)) return LineClass::hr;
    if (heading(line)) return LineClass::heading;
    if (list_item(line)) return LineClass::list;
    if (quote_line(line)) return LineClass::quote;
    if (table_sep(line)) return LineClass::table_sep;
    return LineClass::text;
}

Line& ensure_line(std::vector<Line>& out, size_t i) {
    if (out.size() <= i) out.resize(i + 1);
    return out[i];
}

// 复用的 span 写入器：原地覆盖 Line 的 spans，保留 string 容量。
class SpanBuilder {
public:
    explicit SpanBuilder(Line& ln) noexcept : ln_(&ln) {}
    void add(std::string_view text, const Style& style) {
        if (text.empty()) return;
        if (idx_ == ln_->spans.size()) ln_->spans.emplace_back();
        Span& sp = ln_->spans[idx_++];
        sp.text.assign(text);
        sp.style = style;
    }
    void finish() { ln_->spans.resize(idx_); }

private:
    Line* ln_;
    size_t idx_ = 0;
};

// ---- 表格 ----

struct TableCell {
    std::string text; // tab 已展开
    int width = 0;
};

struct TableRow {
    std::vector<TableCell> cells;
    size_t offset = 0;
    bool separator = false;
};

std::string_view trim_spaces(std::string_view s) noexcept {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && is_space(s[a])) ++a;
    while (b > a && is_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

int display_width(std::string_view s) noexcept {
    int col = 0;
    size_t i = 0;
    while (i < s.size()) {
        std::string_view rest = s.substr(i);
        unicode::Grapheme g;
        if (!unicode::next_grapheme(rest, g)) break;
        col += g.width;
        i += g.bytes.size();
    }
    return col;
}

bool separator_cell(std::string_view s) noexcept {
    bool dash = false;
    for (char c : s) {
        if (c == '-') {
            dash = true;
        } else if (c != ':' && !is_space(c)) {
            return false;
        }
    }
    return dash;
}

void parse_table_row(std::string_view line, size_t offset,
                     std::vector<TableRow>& rows) {
    TableRow row;
    row.offset = offset;
    std::string cell;
    bool escaped = false;
    auto push_cell = [&] {
        TableCell tc;
        expand_row(tc.text, cell, 0, cell.size());
        tc.text = std::string(trim_spaces(tc.text));
        tc.width = display_width(tc.text);
        row.cells.push_back(std::move(tc));
        cell.clear();
    };
    for (char c : line) {
        if (escaped) {
            cell += c;
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '|') {
            push_cell();
            continue;
        }
        cell += c;
    }
    push_cell();
    if (!row.cells.empty() && row.cells.front().text.empty()) {
        row.cells.erase(row.cells.begin());
    }
    if (!row.cells.empty() && row.cells.back().text.empty()) {
        row.cells.pop_back();
    }
    row.separator = !row.cells.empty();
    for (const TableCell& c : row.cells) {
        if (!separator_cell(c.text)) {
            row.separator = false;
            break;
        }
    }
    rows.push_back(std::move(row));
}

std::vector<TableRow> parse_table(std::string_view src) {
    std::vector<TableRow> rows;
    size_t pos = 0;
    while (pos < src.size()) {
        const size_t start = pos;
        const size_t nl = src.find('\n', pos);
        std::string_view line =
            nl == std::string_view::npos ? src.substr(pos) : src.substr(pos, nl - pos);
        pos = nl == std::string_view::npos ? src.size() : nl + 1;
        line = trim_spaces(drop_cr(line));
        if (line.empty()) continue;
        parse_table_row(line, start, rows);
    }
    return rows;
}

size_t count_table_lines(std::string_view src, size_t from) noexcept {
    size_t n = 0;
    size_t pos = from;
    while (pos < src.size()) {
        const size_t nl = src.find('\n', pos);
        const std::string_view line =
            nl == std::string_view::npos ? src.substr(pos) : src.substr(pos, nl - pos);
        pos = nl == std::string_view::npos ? src.size() : nl + 1;
        if (!trim_spaces(drop_cr(line)).empty()) ++n;
    }
    return n;
}

// 取显示宽度不超过 cols 的字素前缀。
std::string_view take_cols(std::string_view s, int cols) noexcept {
    if (cols <= 0) return {};
    int col = 0;
    size_t i = 0;
    while (i < s.size()) {
        std::string_view rest = s.substr(i);
        unicode::Grapheme g;
        if (!unicode::next_grapheme(rest, g)) break;
        if (col + g.width > cols) break;
        col += g.width;
        i += g.bytes.size();
    }
    return s.substr(0, i);
}

// ---- Markdown 排版（§3.7.2）----
//
// 行内标记（** * _ ` [文本](地址) 与反斜杠转义）显示时隐藏，显示文本因此
// 比原文短：计数与物化必须走同一个排版函数 layout_markdown，按显示文本
// 折行，两边的行数才一致。
//
// 排版以"段"为单位：一个块级起始行（段落首行、标题、列表项、引用行）加上
// 其后的续行（普通文本行并入前面的段落、列表项或引用，即 CommonMark 的
// 软换行与懒续行，在显示文本里保留为 '\n'）。行内标记在整段内配对，折行
// 与软换行都不会把它切断。块级前缀用 dim：标题保留 #，列表符号换成 •，
// 引用换成 │；续行对齐到正文起点。

enum class RenderKind : uint8_t { text, heading, list, quote, hr };

// 显示文本上的同样式区间：显示起点、对应的合并原文位置、样式。区间内
// 显示字符与合并原文逐字节对应。
struct MdRun {
    size_t disp = 0;
    size_t comb = 0;
    Style style{};
};

// 合并原文到 source 字节偏移的分段映射（每个原文行一段）。
struct MdPiece {
    size_t comb = 0;
    size_t src = 0;
};

struct MdSegment {
    RenderKind kind = RenderKind::text;
    size_t src_begin = 0; // 段首行在 source 中的偏移（首行的 Line::offset）
    std::string prefix;   // 首行前缀
    std::string cont;     // 续行前缀
    int prefix_w = 0;
    int cont_w = 0;
    Style base{};
    std::string comb;     // 合并原文：去块级前缀，续行去前导空白，tab → 空格
    std::vector<MdPiece> pieces;
    std::string disp;     // 隐藏行内标记后的显示文本
    std::vector<MdRun> runs;

    void reset(size_t begin) {
        kind = RenderKind::text;
        src_begin = begin;
        prefix.clear();
        cont.clear();
        prefix_w = 0;
        cont_w = 0;
        base = Style{};
        comb.clear();
        pieces.clear();
        disp.clear();
        runs.clear();
    }

    void add_piece(size_t src, std::string_view content) {
        pieces.push_back({comb.size(), src});
        for (const char c : content) comb += c == '\t' ? ' ' : c;
    }

    // 显示位置 → source 字节偏移（锚点解析用；段内单调不减）。
    size_t src_of(size_t d) const noexcept {
        if (runs.empty()) return src_begin;
        auto r = std::upper_bound(runs.begin(), runs.end(), d,
                                  [](size_t v, const MdRun& x) { return v < x.disp; });
        --r;
        const size_t c = r->comb + (d - r->disp);
        auto p = std::upper_bound(pieces.begin(), pieces.end(), c,
                                  [](size_t v, const MdPiece& x) { return v < x.comb; });
        --p;
        return p->src + (c - p->comb);
    }
};

// 读一个原文行：text 不含行尾换行与 '\r'，next 是下一行起点。
struct SrcLine {
    std::string_view text;
    size_t begin = 0;
    size_t next = 0;
};

SrcLine read_line(std::string_view src, size_t pos) noexcept {
    const size_t nl = src.find('\n', pos);
    const size_t end = nl == std::string_view::npos ? src.size() : nl;
    return {drop_cr(src.substr(pos, end - pos)), pos,
            nl == std::string_view::npos ? src.size() : nl + 1};
}

size_t skip_spaces(std::string_view s, size_t i) noexcept {
    while (i < s.size() && is_space(s[i])) ++i;
    return i;
}

bool punct(char c) noexcept {
    return std::ispunct(static_cast<unsigned char>(c)) != 0;
}

bool alnum(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

bool inline_special(char c) noexcept {
    return c == '\\' || c == '`' || c == '*' || c == '_' || c == '[';
}

// 行内解析：在合并原文 [lo, hi) 上产出显示文本与样式区间。标记配对遵循
// CommonMark 的简化规则：开标记后不能是空白，闭标记前不能是空白，'_'
// 不在词内生效；配不上对的标记按字面输出。嵌套深度受限。
class InlineParser {
public:
    InlineParser(MdSegment& seg, const Theme& theme) noexcept
        : seg_(seg), theme_(theme) {}

    void parse(size_t lo, size_t hi, const Style& base, int depth) {
        const std::string_view s = seg_.comb;
        size_t i = lo;
        while (i < hi) {
            const char c = s[i];
            if (c == '\\' && i + 1 < hi && punct(s[i + 1])) {
                add(i + 1, 1, base);
                i += 2;
                continue;
            }
            if (c == '`') {
                const size_t n = run_length(i, hi, '`');
                const size_t close = find_backticks(i + n, hi, n);
                if (close != k_npos) {
                    add(i + n, close - i - n, theme_.code);
                    i = close + n;
                } else {
                    add(i, n, base); // 没有等长的闭合反引号：按字面
                    i += n;
                }
                continue;
            }
            if ((c == '*' || c == '_') && depth < 3) {
                const size_t n = run_length(i, hi, c);
                if (n >= 2 && opens(i, 2, hi)) {
                    const size_t j = find_close(i + 2, hi, c, 2);
                    if (j != k_npos) {
                        Style st = base;
                        st.attrs = st.attrs | Attr::bold;
                        parse(i + 2, j, st, depth + 1);
                        i = j + 2;
                        continue;
                    }
                }
                if (opens(i, 1, hi)) {
                    const size_t j = find_close(i + 1, hi, c, 1);
                    if (j != k_npos) {
                        Style st = base;
                        st.attrs = st.attrs | Attr::italic;
                        parse(i + 1, j, st, depth + 1);
                        i = j + 1;
                        continue;
                    }
                }
                add(i, n, base);
                i += n;
                continue;
            }
            if (c == '[' && depth < 3) {
                const size_t j = s.find(']', i + 1);
                if (j != std::string_view::npos && j + 1 < hi && s[j + 1] == '(') {
                    const size_t k = s.find(')', j + 2);
                    if (k != std::string_view::npos && k < hi && k > j + 2) {
                        Style st = base;
                        st.attrs = st.attrs | Attr::underline;
                        parse(i + 1, j, st, depth + 1); // 地址隐藏，只显示链接文本
                        i = k + 1;
                        continue;
                    }
                }
            }
            size_t j = i + 1;
            while (j < hi && !inline_special(s[j])) ++j;
            add(i, j - i, base);
            i = j;
        }
    }

private:
    // 追加合并原文 [at, at+len) 到显示文本；与上一区间同样式且原文连续时合并。
    void add(size_t at, size_t len, const Style& st) {
        if (len == 0) return;
        if (!seg_.runs.empty()) {
            const MdRun& r = seg_.runs.back();
            if (r.style == st && r.comb + (seg_.disp.size() - r.disp) == at) {
                seg_.disp.append(seg_.comb, at, len);
                return;
            }
        }
        seg_.runs.push_back({seg_.disp.size(), at, st});
        seg_.disp.append(seg_.comb, at, len);
    }

    size_t run_length(size_t i, size_t hi, char c) const noexcept {
        size_t j = i;
        while (j < hi && seg_.comb[j] == c) ++j;
        return j - i;
    }

    size_t find_backticks(size_t from, size_t hi, size_t n) const noexcept {
        size_t j = from;
        while (j < hi) {
            if (seg_.comb[j] != '`') {
                ++j;
                continue;
            }
            const size_t m = run_length(j, hi, '`');
            if (m == n) return j;
            j += m;
        }
        return k_npos;
    }

    // i 处的 n 个定界符能否开启强调：其后非空白，'_' 不在词内。
    bool opens(size_t i, size_t n, size_t hi) const noexcept {
        const std::string_view s = seg_.comb;
        if (i + n >= hi) return false;
        const char after = s[i + n];
        if (is_space(after) || after == '\n') return false;
        return s[i] != '_' || i == 0 || !alnum(s[i - 1]);
    }

    // 从 from 起找能闭合的 n 个定界符（n = 1 只认单个，n = 2 认 >= 2 的游程）。
    size_t find_close(size_t from, size_t hi, char c, size_t n) const noexcept {
        const std::string_view s = seg_.comb;
        size_t j = from;
        while (j < hi) {
            if (s[j] != c) {
                ++j;
                continue;
            }
            const size_t m = run_length(j, hi, c);
            const bool fits = n == 1 ? m == 1 : m >= 2;
            const char before = s[j - 1];
            const bool closes = j > from && !is_space(before) && before != '\n' &&
                                (c != '_' || j + m >= hi || !alnum(s[j + m]));
            if (fits && closes) return j;
            j += m;
        }
        return k_npos;
    }

    MdSegment& seg_;
    const Theme& theme_;
};

// 引用行的层数与正文起点（"> > x" 为 2 层）。
size_t quote_depth(std::string_view t, size_t& content) noexcept {
    size_t i = leading_spaces(t);
    size_t levels = 0;
    while (i < t.size() && t[i] == '>') {
        ++levels;
        ++i;
        if (i < t.size() && t[i] == ' ') ++i;
        const size_t j = skip_spaces(t, i);
        if (j < t.size() && t[j] == '>') i = j;
    }
    content = i;
    return levels;
}

std::string spaces(int n) { return std::string(static_cast<size_t>(std::max(n, 0)), ' '); }

// 从 pos 起建一个段，返回下一段的起点。
size_t build_segment(std::string_view src, size_t pos, const Theme& theme,
                     MdSegment& seg) {
    seg.reset(pos);
    const SrcLine first = read_line(src, pos);
    const std::string_view t = first.text;
    const LineClass k = classify(t);
    size_t content = 0; // 首行正文在 t 中的起点
    size_t levels = 0;  // 引用层数
    seg.base = theme.text;

    switch (k) {
    case LineClass::blank:
        return first.next; // 空行：一个空显示行
    case LineClass::hr:
        seg.kind = RenderKind::hr;
        return first.next;
    case LineClass::heading: {
        const size_t i = leading_spaces(t);
        size_t j = i;
        while (j < t.size() && t[j] == '#') ++j;
        seg.kind = RenderKind::heading;
        seg.prefix.assign(t.substr(i, j - i));
        seg.prefix += ' ';
        seg.cont = spaces(display_width(seg.prefix));
        seg.base.attrs = seg.base.attrs | Attr::bold;
        content = j < t.size() ? j + 1 : j;
        break;
    }
    case LineClass::list: {
        const size_t i = skip_spaces(t, 0);
        size_t j = i;
        seg.kind = RenderKind::list;
        seg.prefix = spaces(static_cast<int>(i));
        if (t[i] == '-' || t[i] == '*' || t[i] == '+') {
            seg.prefix += "•";
            j = i + 1;
        } else {
            while (j < t.size() && t[j] >= '0' && t[j] <= '9') ++j;
            ++j; // '.' 或 ')'
            seg.prefix.append(t.substr(i, j - i));
        }
        seg.prefix += ' ';
        seg.cont = spaces(display_width(seg.prefix));
        content = j < t.size() ? j + 1 : j;
        break;
    }
    case LineClass::quote:
        seg.kind = RenderKind::quote;
        levels = quote_depth(t, content);
        for (size_t i = 0; i < levels; ++i) seg.prefix += "│ ";
        seg.cont = seg.prefix;
        seg.base = theme.dim;
        break;
    default:
        content = skip_spaces(t, 0);
        break;
    }
    seg.prefix_w = display_width(seg.prefix);
    seg.cont_w = display_width(seg.cont);
    seg.add_piece(first.begin + content, t.substr(std::min(content, t.size())));

    // 续行：普通文本行并入段落、列表项与引用（懒续行），同层引用行并入
    // 引用；标题只占一行。
    size_t next = first.next;
    if (seg.kind != RenderKind::heading) {
        while (next < src.size()) {
            const SrcLine ln = read_line(src, next);
            const LineClass lk = classify(ln.text);
            size_t ws = 0;
            if (lk == LineClass::quote && seg.kind == RenderKind::quote) {
                size_t c = 0;
                if (quote_depth(ln.text, c) != levels) break;
                ws = skip_spaces(ln.text, c);
            } else if (lk == LineClass::text || lk == LineClass::table_sep) {
                ws = skip_spaces(ln.text, 0);
            } else {
                break;
            }
            seg.comb += '\n';
            seg.add_piece(ln.begin + ws, ln.text.substr(ws));
            next = ln.next;
        }
    }
    InlineParser(seg, theme).parse(0, seg.comb.size(), seg.base, 0);
    return next;
}

// 逐段排版，每个显示行回调一次 row(段, 是否段首行, 显示起点, 显示终点, 行宽)。
template <class Row>
void layout_markdown(std::string_view src, int width, const Theme& theme,
                     MdSegment& seg, Row&& row) {
    size_t pos = 0;
    while (pos < src.size()) {
        pos = build_segment(src, pos, theme, seg);
        if (seg.kind == RenderKind::hr) {
            row(seg, true, 0, 0, std::max(width, 1));
            continue;
        }
        size_t p = 0;
        bool first = true;
        for (;;) {
            const int pw = first ? seg.prefix_w : seg.cont_w;
            const RowEdge e = wrap_next_row(seg.disp, p, std::max(1, width - pw));
            row(seg, first, p, e.end, pw + e.width);
            first = false;
            p = e.next;
            if (p >= seg.disp.size()) break;
        }
    }
}

} // namespace

// ---- MarkdownStream ----

void MarkdownStream::feed(std::string_view chunk) {
    size_t i = 0;
    while (i < chunk.size()) {
        const size_t nl = chunk.find('\n', i);
        if (nl == std::string_view::npos) {
            pending_.append(chunk.substr(i));
            on_partial();
            break;
        }
        pending_.append(chunk.substr(i, nl - i));
        on_partial(); // 行尾之前的字节也要先落到当前块
        on_line(true);
        pending_.clear();
        sent_ = 0;
        line_in_block_ = false;
        line_new_ = false;
        if (mode_ == Mode::code) code_candidate_ = true; // 新行可能是闭合围栏
        i = nl + 1;
    }
}

// pending_ 增长但尚无换行：能确定的（普通段落行）立即建块显示；
// 可能是围栏/表格开头的字符先攒着，整行到达后再判定。
void MarkdownStream::on_partial() {
    if (staging_) return;
    if (mode_ == Mode::none) {
        size_t i = 0;
        while (i < pending_.size() && is_space(pending_[i])) ++i;
        if (i >= pending_.size()) return;
        const char c = pending_[i];
        if (c == '`' || c == '~' || c == '|') return;
        Block b = new_block(BlockKind::markdown);
        b.open = true;
        cur_ = doc_->append_block(std::move(b));
        mode_ = Mode::markdown;
        family_ = Family::text;
        line_new_ = true;
        doc_->append(cur_, pending_);
        sent_ = pending_.size();
        line_in_block_ = true;
        return;
    }
    if (mode_ == Mode::code && code_candidate_) {
        if (could_close_fence(pending_, fence_ch_)) return;
        code_candidate_ = false;
    }
    doc_->append(cur_, std::string_view(pending_).substr(sent_));
    sent_ = pending_.size();
    line_in_block_ = true;
}

// pending_ 是一整行（has_newline 时行尾有 '\n'，finish 的收尾行没有）。
void MarkdownStream::on_line(bool has_newline) {
    std::string_view raw = pending_;
    const std::string_view line = drop_cr(raw);
    const size_t raw_len = raw.size() + (has_newline ? 1 : 0);

    if (staging_) {
        // 暂存的表头行已含换行；本行确认表格或否定它。
        if (table_sep(line)) {
            std::string src = std::move(staged_);
            staged_.clear();
            staging_ = false;
            src.append(raw);
            if (has_newline) src += '\n';
            open_table(std::move(src));
            return;
        }
        open_markdown(std::move(staged_), /*pipe=*/true);
        staged_.clear();
        staging_ = false;
        if (!raw.empty()) {
            doc_->append(cur_, raw);
            sent_ = raw.size();
            line_in_block_ = true;
        }
        handle_markdown_line(line, has_newline);
        return;
    }

    if (mode_ == Mode::none) {
        handle_detached_line(line, has_newline, /*allow_table=*/true);
        return;
    }

    if (mode_ == Mode::code) {
        if (has_newline && fence_close(line, fence_ch_, fence_len_)) {
            close_current(); // 闭合围栏不属于 source
            return;
        }
        if (line_in_block_) {
            if (has_newline) doc_->append(cur_, "\n");
        } else {
            doc_->append(cur_, raw);
            if (has_newline) doc_->append(cur_, "\n");
            line_in_block_ = true;
        }
        return;
    }

    if (mode_ == Mode::table) {
        if (blank(line) || !starts_pipe(line)) {
            if (line_in_block_) {
                if (has_newline) doc_->append(cur_, "\n");
                remove_last(raw_len);
            }
            close_current();
            handle_detached_line(line, has_newline, /*allow_table=*/false);
            return;
        }
        if (line_in_block_) {
            if (has_newline) doc_->append(cur_, "\n");
        } else {
            doc_->append(cur_, raw);
            if (has_newline) doc_->append(cur_, "\n");
            line_in_block_ = true;
        }
        return;
    }

    handle_markdown_line(line, has_newline);
}

// 行内容已在当前 markdown 块中；需要时先补行尾换行，再按结构决定去留。
void MarkdownStream::handle_markdown_line(std::string_view line,
                                          bool has_newline) {
    const size_t raw_len = pending_.size() + (has_newline ? 1 : 0);
    char fence = 0;
    size_t fence_len = 0;
    std::string fence_info;
    if (fence_open(line, fence, fence_len, fence_info)) {
        if (has_newline) doc_->append(cur_, "\n");
        remove_last(raw_len);
        close_current();
        start_code(fence, fence_len, std::move(fence_info));
        return;
    }
    const LineClass k = classify(line);

    if (k == LineClass::blank) {
        // 空行不进块：只回退本行已显示的空白（通常没有），不做整块替换。
        if (!pending_.empty()) remove_last(pending_.size());
        close_current();
        return;
    }
    if (k == LineClass::hr) {
        if (has_newline) doc_->append(cur_, "\n");
        if (line_new_) {
            close_current(); // 块由本行开启：分隔线自成一等，不回退
            return;
        }
        remove_last(raw_len);
        close_current();
        Block b = new_block(BlockKind::markdown);
        b.source = pending_;
        if (has_newline) b.source += '\n';
        doc_->append_block(std::move(b)); // 立即关闭
        return;
    }
    if (k == LineClass::table_sep && last_line_pipe_) {
        if (has_newline) doc_->append(cur_, "\n");
        const size_t n = raw_len + last_line_len_;
        const Block& b = *doc_->find(cur_);
        std::string table_src = b.source.substr(b.source.size() - n);
        remove_last(n);
        close_current();
        open_table(std::move(table_src));
        return;
    }

    Family fam = k == LineClass::heading  ? Family::heading
                 : k == LineClass::list   ? Family::list
                 : k == LineClass::quote  ? Family::quote
                                          : Family::text;
    // 列表项与引用的续行（缩进续行或懒续行）没有开启新的块级结构，
    // 留在当前块；渲染器把它并入前一个列表项/引用。
    if (fam == Family::text &&
        (family_ == Family::list || family_ == Family::quote)) {
        fam = family_;
    }
    if (!line_new_ && fam != family_) {
        if (has_newline) doc_->append(cur_, "\n");
        remove_last(raw_len);
        close_current();
        std::string src(pending_);
        if (has_newline) src += '\n';
        open_markdown(std::move(src), starts_pipe(line));
        family_ = fam;
        return;
    }
    if (has_newline) doc_->append(cur_, "\n");
    family_ = fam;
    last_line_pipe_ = starts_pipe(line);
    last_line_len_ = raw_len;
    line_new_ = false;
}

// 行不在任何块中（模式 none，或表格刚结束）：整行决定新块。
void MarkdownStream::handle_detached_line(std::string_view line,
                                          bool has_newline, bool allow_table) {
    char fence = 0;
    size_t fence_len = 0;
    std::string fence_info;
    if (fence_open(line, fence, fence_len, fence_info)) {
        start_code(fence, fence_len, std::move(fence_info));
        return;
    }
    const LineClass k = classify(line);
    if (k == LineClass::blank) return;
    std::string src(pending_);
    if (has_newline) src += '\n';
    if (allow_table && starts_pipe(line)) {
        staged_ = std::move(src); // 表头需要下一行确认
        staging_ = true;
        return;
    }
    if (k == LineClass::hr) {
        Block b = new_block(BlockKind::markdown);
        b.source = std::move(src);
        doc_->append_block(std::move(b)); // 立即关闭
        return;
    }
    open_markdown(std::move(src), starts_pipe(line));
    family_ = k == LineClass::heading  ? Family::heading
              : k == LineClass::list   ? Family::list
              : k == LineClass::quote  ? Family::quote
                                       : Family::text;
}

void MarkdownStream::open_markdown(std::string source, bool pipe) {
    Block b = new_block(BlockKind::markdown);
    b.source = std::move(source);
    b.open = true;
    last_line_len_ = b.source.size();
    cur_ = doc_->append_block(std::move(b));
    mode_ = Mode::markdown;
    family_ = Family::text;
    last_line_pipe_ = pipe;
    line_in_block_ = false;
    line_new_ = false;
}

void MarkdownStream::open_table(std::string source) {
    Block b = new_block(BlockKind::table);
    b.source = std::move(source);
    b.open = true;
    cur_ = doc_->append_block(std::move(b));
    mode_ = Mode::table;
    line_in_block_ = false;
    line_new_ = false;
}

void MarkdownStream::start_code(char fence, size_t len, std::string info) {
    Block b = new_block(BlockKind::code);
    b.meta = std::move(info);
    b.open = true;
    cur_ = doc_->append_block(std::move(b));
    mode_ = Mode::code;
    fence_ch_ = fence;
    fence_len_ = len;
    code_candidate_ = false;
    line_in_block_ = false;
    line_new_ = false;
}

// 新块的上边距：消息内第一块用 first_margin_（消息之间的间距归应用），
// 其后每块空 1 行（段落间距）。
Block MarkdownStream::new_block(BlockKind kind) {
    Block b;
    b.kind = kind;
    b.margin_top = started_ ? 1 : first_margin_;
    started_ = true;
    return b;
}

void MarkdownStream::close_current() {
    if (mode_ != Mode::none && cur_ != 0) doc_->close_block(cur_);
    mode_ = Mode::none;
    cur_ = 0;
    code_candidate_ = false;
    line_in_block_ = false;
    line_new_ = false;
}

void MarkdownStream::remove_last(size_t bytes) {
    const Block* b = doc_->find(cur_);
    if (b == nullptr) return;
    const size_t keep = b->source.size() > bytes ? b->source.size() - bytes : 0;
    doc_->replace(cur_, b->source.substr(0, keep));
}

void MarkdownStream::finish() {
    std::string_view raw = pending_;
    const std::string_view line = drop_cr(raw);

    if (staging_) {
        if (!raw.empty() && table_sep(line)) {
            std::string src = std::move(staged_);
            src.append(raw);
            open_table(std::move(src));
        } else {
            open_markdown(std::move(staged_), /*pipe=*/true);
            if (!raw.empty()) {
                doc_->append(cur_, raw);
                sent_ = raw.size();
                line_in_block_ = true;
                handle_markdown_line(line, false);
            }
        }
        staged_.clear();
        staging_ = false;
    } else if (!raw.empty()) {
        if (mode_ == Mode::none) {
            // 末尾没有下一行可确认，表格不可能成立。
            handle_detached_line(line, false, /*allow_table=*/false);
        } else if (mode_ == Mode::code) {
            if (!fence_close(line, fence_ch_, fence_len_) && !line_in_block_) {
                doc_->append(cur_, raw);
                line_in_block_ = true;
            }
        } else if (mode_ == Mode::table) {
            if (blank(line) || !starts_pipe(line)) {
                if (line_in_block_) remove_last(raw.size());
                close_current();
                handle_detached_line(line, false, /*allow_table=*/false);
            }
        } else {
            handle_markdown_line(line, false);
        }
    }
    pending_.clear();
    sent_ = 0;
    line_in_block_ = false;
    line_new_ = false;
    close_current();
    started_ = false; // 下一条消息重新从第一块开始
}

// ---- MarkdownRenderer ----

// 行内标记隐藏后行数取决于整段解析，而强调可跨软换行：stable_rows = 0，
// 每帧从块首整块排版（代价受段落长度约束，§3.7.1）。计数与物化共用
// layout_markdown，行数必然一致。封闭块（stable_bytes = 末尾）重数时增量为零。
WrapResult MarkdownRenderer::measure(std::string_view source, size_t from,
                                     int width) const {
    if (from >= source.size()) return {};
    thread_local MdSegment seg; // 复用容量：流式帧路径不反复分配
    static const Theme theme{};  // 计数只看显示文本，与样式无关
    size_t rows = 0;
    layout_markdown(source.substr(from), width, theme, seg,
                    [&](const MdSegment&, bool, size_t, size_t, int) { ++rows; });
    return {rows, 0, 0};
}

size_t MarkdownRenderer::render(const Block& block, int width, const Theme& theme,
                                size_t from, size_t valid,
                                std::vector<Line>& out) const {
    (void)from; // stable_rows = 0：总是整块重排
    (void)valid;
    const size_t limit = block.collapsed
                             ? std::min<size_t>(block.collapsed_rows, block.row_count)
                             : block.row_count;
    thread_local MdSegment seg;
    size_t n = 0;
    layout_markdown(
        block.source, width, theme, seg,
        [&](const MdSegment& sg, bool first, size_t b, size_t e, int w) {
            if (n >= limit) return;
            Line& ln = ensure_line(out, n);
            SpanBuilder sb(ln);
            if (sg.kind == RenderKind::hr) {
                std::string rule;
                for (int i = 0; i < w; ++i) rule += "─";
                sb.add(rule, theme.dim);
            } else {
                sb.add(first ? sg.prefix : sg.cont, theme.dim);
                if (b < e) {
                    auto r = std::upper_bound(
                        sg.runs.begin(), sg.runs.end(), b,
                        [](size_t v, const MdRun& x) { return v < x.disp; });
                    --r;
                    for (; r != sg.runs.end() && r->disp < e; ++r) {
                        const size_t rb = std::max(b, r->disp);
                        const size_t re =
                            std::min(e, r + 1 != sg.runs.end() ? (r + 1)->disp
                                                               : sg.disp.size());
                        sb.add(std::string_view(sg.disp).substr(rb, re - rb),
                               r->style);
                    }
                }
            }
            sb.finish();
            ln.width = w;
            ln.offset = first ? sg.src_begin : sg.src_of(b);
            ln.lex = 0;
            ++n;
        });
    return n;
}

// ---- TableRenderer ----

// 列宽依赖全部行，stable_rows = 0：每次变化整块重排。行数从 from 起数，
// 封闭块（stable_bytes = 末尾）重数时增量为零。
WrapResult TableRenderer::measure(std::string_view source, size_t from,
                                  int width) const {
    (void)width;
    return {count_table_lines(source, from), 0, 0};
}

size_t TableRenderer::render(const Block& block, int width, const Theme& theme,
                             size_t from, size_t valid,
                             std::vector<Line>& out) const {
    (void)from;
    (void)valid;
    const std::vector<TableRow> rows = parse_table(block.source);
    const size_t limit = block.collapsed
                             ? std::min<size_t>(block.collapsed_rows, block.row_count)
                             : block.row_count;

    size_t cols = 0;
    for (const TableRow& r : rows) cols = std::max(cols, r.cells.size());
    std::vector<int> natural(cols, 1);
    for (const TableRow& r : rows) {
        if (r.separator) continue;
        for (size_t j = 0; j < r.cells.size(); ++j) {
            natural[j] = std::max(natural[j], r.cells[j].width);
        }
    }

    // 列宽：总宽放得下就用自然宽度，否则按比例收缩到可用行宽。
    std::vector<int> colw(cols, 1);
    long long sum = 0;
    for (int w : natural) sum += w;
    long long room = static_cast<long long>(width) - 1 - 3 * static_cast<long long>(cols);
    if (room >= sum) {
        colw = natural;
    } else {
        if (room < static_cast<long long>(cols)) room = static_cast<long long>(cols);
        const long long extra = room - static_cast<long long>(cols);
        long long used = 0;
        for (size_t j = 0; j < cols; ++j) {
            const long long e =
                sum > 0 ? static_cast<long long>(natural[j]) * extra / sum : 0;
            colw[j] = 1 + static_cast<int>(e);
            used += e;
        }
        for (size_t j = 0; used < extra; ++j, ++used) ++colw[j % cols];
    }
    int total_w = 1 + 3 * static_cast<int>(cols);
    for (int w : colw) total_w += w;

    const Style border = theme.dim;
    const Style body = theme.text;
    Style header = theme.text;
    header.attrs = header.attrs | Attr::bold;

    size_t n = 0;
    bool first_content = true;
    std::string buf;
    for (const TableRow& r : rows) {
        if (n >= limit) break;
        Line& ln = ensure_line(out, n);
        SpanBuilder w(ln);
        if (r.separator) {
            std::string rule = "├";
            for (size_t j = 0; j < cols; ++j) {
                for (int k = 0; k < colw[j] + 2; ++k) rule += "─";
                rule += j + 1 < cols ? "┼" : "┤";
            }
            w.add(rule, border);
        } else {
            const Style& st = first_content ? header : body;
            first_content = false;
            w.add("│", border);
            for (size_t j = 0; j < cols; ++j) {
                buf.clear();
                if (j < r.cells.size()) {
                    const TableCell& c = r.cells[j];
                    if (c.width <= colw[j]) {
                        buf = c.text;
                        buf.append(static_cast<size_t>(colw[j] - c.width), ' ');
                    } else {
                        buf = take_cols(c.text, colw[j] - 1);
                        buf += "…";
                    }
                } else {
                    buf.append(static_cast<size_t>(colw[j]), ' ');
                }
                w.add(" ", border);
                w.add(buf, st);
                w.add(" ", border);
                w.add("│", border);
            }
        }
        w.finish();
        ln.width = total_w;
        ln.offset = r.offset;
        ln.lex = 0;
        ++n;
    }
    return n;
}

} // namespace dagent::tui
