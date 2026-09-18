// L5 轻量语法高亮（§3.7.3）。
//
// 一行扫描器：按语言配置识别行注释、块注释、字符串（含可跨行的 Python
// 三引号、JS 模板串、Go/Rust 原始串、Bash 引号、Rust 生命周期）、数字、
// 关键字、函数名与类型名。跨行词法状态存进 Line::lex，增量重扫从上一
// 有效行继续，流式追加只处理最后一行。
//
// 不引入 tree-sitter：语法覆盖以"轻量、够用"为界，未识别的语言按主题
// 的 code 槽纯文本输出。样式取自现有 Theme 槽位与文本属性；§3.12 的
// 语义令牌落地后只需替换这里的映射。
#include "tui/document.hpp"

#include "tui/grapheme.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dagent::tui {

namespace {

enum class Lang : uint8_t {
    plain,
    c,
    python,
    javascript,
    json,
    bash,
    go,
    rust,
};

struct Spec {
    bool hash_comment = false;
    bool slash_comment = false;
    bool block_comment = false;
    bool nested_block = false;
    bool triple_quote = false;
    bool backtick = false;
    bool rust_raw = false;
    bool char_literal = false;
    bool shell_quote = false;
    bool preproc = false;
};

// Line::lex 的打包：低 4 位模式，高 4 位参数（三引号类型、注释嵌套深度、
// 原始字符串的 # 数）。
enum : uint8_t {
    m_normal = 0,
    m_block_comment = 1,
    m_triple = 2,
    m_backtick = 3,
    m_shell_single = 4,
    m_shell_double = 5,
    m_rust_raw = 6,
};

bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }

bool is_ident_start(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_ident_char(char c) noexcept {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

char next_non_space(std::string_view s, size_t j) noexcept {
    while (j < s.size() && is_space(s[j])) ++j;
    return j < s.size() ? s[j] : '\0';
}

constexpr std::string_view k_c_keywords[] = {
    "alignas", "alignof", "asm", "auto", "bool", "break", "case", "catch",
    "char", "class", "const", "const_cast", "constexpr", "continue", "decltype",
    "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
    "explicit", "export", "extern", "false", "final", "float", "for", "friend",
    "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
    "noexcept", "nullptr", "operator", "override", "private", "protected",
    "public", "register", "reinterpret_cast", "return", "short", "signed",
    "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
    "template", "this", "throw", "true", "try", "typedef", "typeid",
    "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
    "wchar_t", "while",
};

constexpr std::string_view k_py_keywords[] = {
    "False", "None", "True", "and", "as", "assert", "async", "await", "break",
    "class", "cls", "continue", "def", "del", "elif", "else", "except",
    "finally", "for", "from", "global", "if", "import", "in", "is", "lambda",
    "nonlocal", "not", "or", "pass", "raise", "return", "self", "try", "while",
    "with", "yield",
};

constexpr std::string_view k_js_keywords[] = {
    "abstract", "any", "as", "async", "await", "bigint", "boolean", "break",
    "case", "catch", "class", "const", "continue", "debugger", "declare",
    "default", "delete", "do", "else", "enum", "export", "extends", "false",
    "finally", "for", "function", "if", "implements", "import", "in", "infer",
    "instanceof", "interface", "keyof", "let", "namespace", "never", "new",
    "null", "number", "object", "of", "package", "private", "protected",
    "public", "readonly", "return", "satisfies", "static", "string", "super",
    "switch", "symbol", "this", "throw", "true", "try", "type", "typeof",
    "undefined", "unknown", "var", "void", "while", "with", "yield",
};

constexpr std::string_view k_json_keywords[] = {"true", "false", "null"};

constexpr std::string_view k_bash_keywords[] = {
    "alias", "break", "case", "cd", "continue", "coproc", "declare", "do",
    "done", "echo", "elif", "else", "esac", "eval", "exec", "exit", "export",
    "false", "fi", "for", "function", "if", "in", "let", "local", "printf",
    "pwd", "read", "readonly", "return", "select", "set", "shift", "source",
    "test", "then", "time", "trap", "true", "typeset", "unalias", "unset",
    "until", "while",
};

constexpr std::string_view k_go_keywords[] = {
    "any", "bool", "break", "byte", "case", "chan", "complex64", "complex128",
    "const", "continue", "default", "defer", "else", "error", "fallthrough",
    "false", "float32", "float64", "for", "func", "go", "goto", "if", "import",
    "int", "int8", "int16", "int32", "int64", "interface", "iota", "map", "nil",
    "package", "range", "return", "rune", "select", "string", "struct",
    "switch", "true", "type", "uint", "uint8", "uint16", "uint32", "uint64",
    "uintptr", "var",
};

constexpr std::string_view k_rust_keywords[] = {
    "Self", "as", "async", "await", "bool", "box", "break", "char", "const",
    "continue", "crate", "dyn", "else", "enum", "Err", "extern", "f32", "f64",
    "false", "fn", "for", "i128", "i16", "i32", "i64", "i8", "if", "impl",
    "in", "isize", "let", "loop", "macro", "match", "mod", "move", "mut",
    "None", "Ok", "Option", "pub", "ref", "Result", "return", "self", "Some",
    "static", "str", "String", "struct", "super", "trait", "true", "type",
    "u128", "u16", "u32", "u64", "u8", "unsafe", "use", "usize", "Vec",
    "where", "while", "yield",
};

template <size_t N>
bool in_list(std::string_view id, const std::string_view (&list)[N]) noexcept {
    for (size_t i = 0; i < N; ++i) {
        if (list[i] == id) return true;
    }
    return false;
}

bool is_keyword(Lang lang, std::string_view id) noexcept {
    switch (lang) {
    case Lang::c:
        return in_list(id, k_c_keywords);
    case Lang::python:
        return in_list(id, k_py_keywords);
    case Lang::javascript:
        return in_list(id, k_js_keywords);
    case Lang::json:
        return in_list(id, k_json_keywords);
    case Lang::bash:
        return in_list(id, k_bash_keywords);
    case Lang::go:
        return in_list(id, k_go_keywords);
    case Lang::rust:
        return in_list(id, k_rust_keywords);
    default:
        return false;
    }
}

Lang detect_lang(std::string_view meta) noexcept {
    size_t a = 0;
    size_t b = meta.size();
    while (a < b && is_space(meta[a])) ++a;
    while (b > a && is_space(meta[b - 1])) --b;
    std::string_view word = meta.substr(a, b - a);
    const size_t sp = word.find_first_of(" \t");
    if (sp != std::string_view::npos) word = word.substr(0, sp);
    if (word.size() > 24) return Lang::plain;

    char lower[25]{};
    for (size_t i = 0; i < word.size(); ++i) {
        lower[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(word[i])));
    }
    const std::string_view l(lower, word.size());
    if (l == "c" || l == "h" || l == "cpp" || l == "c++" || l == "cc" ||
        l == "cxx" || l == "hpp" || l == "hxx" || l == "objc" ||
        l == "objective-c") {
        return Lang::c;
    }
    if (l == "python" || l == "py") return Lang::python;
    if (l == "javascript" || l == "js" || l == "typescript" || l == "ts" ||
        l == "jsx" || l == "tsx" || l == "node") {
        return Lang::javascript;
    }
    if (l == "json" || l == "jsonc") return Lang::json;
    if (l == "bash" || l == "sh" || l == "shell" || l == "zsh" ||
        l == "console") {
        return Lang::bash;
    }
    if (l == "go" || l == "golang") return Lang::go;
    if (l == "rust" || l == "rs") return Lang::rust;
    return Lang::plain;
}

Spec spec_of(Lang lang) noexcept {
    Spec sp;
    switch (lang) {
    case Lang::c:
        sp.slash_comment = true;
        sp.block_comment = true;
        sp.char_literal = true;
        sp.preproc = true;
        break;
    case Lang::python:
        sp.hash_comment = true;
        sp.triple_quote = true;
        break;
    case Lang::javascript:
        sp.slash_comment = true;
        sp.block_comment = true;
        sp.backtick = true;
        break;
    case Lang::json:
        break;
    case Lang::bash:
        sp.hash_comment = true;
        sp.shell_quote = true;
        break;
    case Lang::go:
        sp.slash_comment = true;
        sp.block_comment = true;
        sp.backtick = true;
        break;
    case Lang::rust:
        sp.slash_comment = true;
        sp.block_comment = true;
        sp.nested_block = true;
        sp.rust_raw = true;
        sp.char_literal = true;
        break;
    default:
        break;
    }
    return sp;
}

struct LexStyles {
    Style text;
    Style keyword;
    Style type;
    Style function;
    Style string;
    Style comment;
    Style number;
};

// 一条逻辑行上的着色区间：行内字节起点 + 样式（终点是下一区间的起点）。
// 词法分析按整条逻辑行进行，折行只按字节范围切片 —— 行注释、字符串、
// 标识符后的 '(' 判定都不会被折行打断。
struct LexRun {
    size_t at = 0;
    Style style{};
};

// lex_row 的输出端：add 收到的都是行内子串，按指针差换算偏移。
class RunCollector {
public:
    void reset(std::string_view line) noexcept {
        base_ = line.data();
        runs_.clear();
    }
    void add(std::string_view text, const Style& style) {
        if (text.empty()) return;
        if (!runs_.empty() && runs_.back().style == style) return; // 相邻同样式合并
        runs_.push_back({static_cast<size_t>(text.data() - base_), style});
    }
    const std::vector<LexRun>& runs() const noexcept { return runs_; }

private:
    const char* base_ = nullptr;
    std::vector<LexRun> runs_;
};

// 物化行的 span 写入器：原地覆盖，保留 string 容量。
class SpanWriter {
public:
    void reset(Line& ln) noexcept {
        ln_ = &ln;
        idx_ = 0;
    }
    std::string& next(const Style& style) {
        if (idx_ == ln_->spans.size()) ln_->spans.emplace_back();
        Span& sp = ln_->spans[idx_++];
        sp.text.clear();
        sp.style = style;
        return sp.text;
    }
    void finish() { ln_->spans.resize(idx_); }

private:
    Line* ln_ = nullptr;
    size_t idx_ = 0;
};

// 与 wrap_next_row / expand_row 相同的列推进：制表符展开到行内相对的
// tab stop（8）。col 跨 span 连续，保证切片后的展开与整行展开一致。
constexpr int k_tab_stop = 8;

void append_expanded(std::string& dst, std::string_view s, int& col) {
    while (!s.empty()) {
        unicode::Grapheme g;
        if (!unicode::next_grapheme(s, g)) break;
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

size_t line_start_of(std::string_view s, size_t pos) noexcept {
    size_t i = pos;
    while (i > 0 && s[i - 1] != '\n' && s[i - 1] != '\r') --i;
    return i;
}

Line& ensure_line(std::vector<Line>& out, size_t i) {
    if (out.size() <= i) out.resize(i + 1);
    return out[i];
}

bool starts_token(const Spec& sp, std::string_view s, size_t i) noexcept {
    const char c = s[i];
    if (is_ident_start(c) || (c >= '0' && c <= '9')) return true;
    if (c == '"' || c == '\'') return true;
    if (c == '`') return sp.backtick;
    if (c == '#') return sp.hash_comment || sp.preproc;
    if (c == '/') {
        if (sp.slash_comment && i + 1 < s.size() && s[i + 1] == '/') return true;
        if (sp.block_comment && i + 1 < s.size() && s[i + 1] == '*') return true;
        return false;
    }
    if (c == '$') return sp.shell_quote;
    return false;
}

void lex_row(Lang lang, const Spec& sp, std::string_view s, uint8_t& state,
             RunCollector& w, const LexStyles& st) {
    const size_t n = s.size();
    size_t i = 0;
    const uint8_t mode = state & 0x0F;
    const uint8_t param = state >> 4;
    state = 0;

    // 上一行延续下来的跨行构造：先找结束定界符，整段按字符串/注释着色。
    if (mode == m_block_comment) {
        size_t j = 0;
        int depth = param > 0 ? param : 1;
        bool closed = false;
        while (j < n) {
            if (sp.nested_block && j + 1 < n && s[j] == '/' && s[j + 1] == '*') {
                ++depth;
                j += 2;
                continue;
            }
            if (j + 1 < n && s[j] == '*' && s[j + 1] == '/') {
                --depth;
                j += 2;
                if (depth == 0) {
                    closed = true;
                    break;
                }
                continue;
            }
            ++j;
        }
        if (!closed) {
            w.add(s, st.comment);
            state = static_cast<uint8_t>(m_block_comment |
                                         (std::min(depth, 15) << 4));
            return;
        }
        w.add(s.substr(0, j), st.comment);
        i = j;
    } else if (mode == m_triple) {
        const std::string_view delim = param ? "'''" : "\"\"\"";
        const size_t p = s.find(delim);
        if (p == std::string_view::npos) {
            w.add(s, st.string);
            state = static_cast<uint8_t>(m_triple | (param << 4));
            return;
        }
        w.add(s.substr(0, p + 3), st.string);
        i = p + 3;
    } else if (mode == m_backtick) {
        const size_t p = s.find('`');
        if (p == std::string_view::npos) {
            w.add(s, st.string);
            state = m_backtick;
            return;
        }
        w.add(s.substr(0, p + 1), st.string);
        i = p + 1;
    } else if (mode == m_shell_single) {
        const size_t p = s.find('\'');
        if (p == std::string_view::npos) {
            w.add(s, st.string);
            state = m_shell_single;
            return;
        }
        w.add(s.substr(0, p + 1), st.string);
        i = p + 1;
    } else if (mode == m_shell_double) {
        size_t j = 0;
        bool closed = false;
        while (j < n) {
            if (s[j] == '\\') {
                j += 2;
                continue;
            }
            if (s[j] == '"') {
                closed = true;
                ++j;
                break;
            }
            ++j;
        }
        if (!closed) {
            w.add(s, st.string);
            state = m_shell_double;
            return;
        }
        w.add(s.substr(0, j), st.string);
        i = j;
    } else if (mode == m_rust_raw) {
        std::string close = "\"";
        close.append(param, '#');
        const size_t p = s.find(close);
        if (p == std::string_view::npos) {
            w.add(s, st.string);
            state = static_cast<uint8_t>(m_rust_raw | (param << 4));
            return;
        }
        w.add(s.substr(0, p + close.size()), st.string);
        i = p + close.size();
    }

    // C/C++ 预处理指令：行首 '#' 之后整行按关键字处理。
    if (sp.preproc) {
        size_t j = i;
        while (j < n && is_space(s[j])) ++j;
        if (j < n && s[j] == '#') {
            w.add(s.substr(i), st.keyword);
            return;
        }
    }

    while (i < n) {
        if (!starts_token(sp, s, i)) {
            size_t j = i + 1;
            while (j < n && !starts_token(sp, s, j)) ++j;
            w.add(s.substr(i, j - i), st.text);
            i = j;
            continue;
        }
        const char c = s[i];

        if (sp.hash_comment && c == '#' && (i == 0 || is_space(s[i - 1]))) {
            w.add(s.substr(i), st.comment);
            return;
        }
        if (sp.slash_comment && c == '/' && i + 1 < n && s[i + 1] == '/') {
            w.add(s.substr(i), st.comment);
            return;
        }
        if (sp.block_comment && c == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t j = i + 2;
            int depth = 1;
            bool closed = false;
            while (j < n) {
                if (sp.nested_block && j + 1 < n && s[j] == '/' &&
                    s[j + 1] == '*') {
                    ++depth;
                    j += 2;
                    continue;
                }
                if (j + 1 < n && s[j] == '*' && s[j + 1] == '/') {
                    --depth;
                    j += 2;
                    if (depth == 0) {
                        closed = true;
                        break;
                    }
                    continue;
                }
                ++j;
            }
            if (!closed) {
                w.add(s.substr(i), st.comment);
                state = static_cast<uint8_t>(m_block_comment |
                                             (std::min(depth, 15) << 4));
                return;
            }
            w.add(s.substr(i, j - i), st.comment);
            i = j;
            continue;
        }
        if (sp.triple_quote && (c == '"' || c == '\'') && i + 2 < n &&
            s[i + 1] == c && s[i + 2] == c) {
            const std::string_view delim = c == '"' ? "\"\"\"" : "'''";
            const size_t p = s.find(delim, i + 3);
            if (p == std::string_view::npos) {
                w.add(s.substr(i), st.string);
                state = static_cast<uint8_t>(
                    m_triple | ((c == '\'' ? 1 : 0) << 4));
                return;
            }
            w.add(s.substr(i, p + 3 - i), st.string);
            i = p + 3;
            continue;
        }
        if (sp.backtick && c == '`') {
            const size_t p = s.find('`', i + 1);
            if (p == std::string_view::npos) {
                w.add(s.substr(i), st.string);
                state = m_backtick;
                return;
            }
            w.add(s.substr(i, p + 1 - i), st.string);
            i = p + 1;
            continue;
        }
        if (sp.rust_raw && c == 'r' && i + 1 < n &&
            (s[i + 1] == '"' || s[i + 1] == '#')) {
            size_t j = i + 1;
            size_t hashes = 0;
            while (j < n && s[j] == '#') {
                ++hashes;
                ++j;
            }
            if (j < n && s[j] == '"') {
                std::string close = "\"";
                close.append(hashes, '#');
                const size_t p = s.find(close, j + 1);
                if (p == std::string_view::npos) {
                    w.add(s.substr(i), st.string);
                    state = static_cast<uint8_t>(
                        m_rust_raw | (std::min<size_t>(hashes, 15) << 4));
                    return;
                }
                w.add(s.substr(i, p + close.size() - i), st.string);
                i = p + close.size();
                continue;
            }
        }
        if (sp.shell_quote && c == '\'') {
            const size_t p = s.find('\'', i + 1);
            if (p == std::string_view::npos) {
                w.add(s.substr(i), st.string);
                state = m_shell_single;
                return;
            }
            w.add(s.substr(i, p + 1 - i), st.string);
            i = p + 1;
            continue;
        }
        if (sp.shell_quote && c == '"') {
            size_t j = i + 1;
            bool closed = false;
            while (j < n) {
                if (s[j] == '\\') {
                    j += 2;
                    continue;
                }
                if (s[j] == '"') {
                    closed = true;
                    ++j;
                    break;
                }
                ++j;
            }
            if (!closed) {
                w.add(s.substr(i), st.string);
                state = m_shell_double;
                return;
            }
            w.add(s.substr(i, j - i), st.string);
            i = j;
            continue;
        }
        if (c == '\'' && sp.char_literal) {
            size_t j = i + 1;
            bool closed = false;
            while (j < n && j - i <= 8) {
                if (s[j] == '\\') {
                    j += 2;
                    continue;
                }
                if (s[j] == '\'') {
                    closed = true;
                    ++j;
                    break;
                }
                ++j;
            }
            if (closed) {
                w.add(s.substr(i, j - i), st.string);
                i = j;
                continue;
            }
            size_t k = i + 1; // Rust 生命周期（'a）
            while (k < n && is_ident_char(s[k])) ++k;
            if (k > i + 1) {
                w.add(s.substr(i, k - i), st.type);
                i = k;
                continue;
            }
            w.add(s.substr(i, 1), st.text);
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            const char q = c;
            size_t j = i + 1;
            bool closed = false;
            while (j < n) {
                if (s[j] == '\\') {
                    j += 2;
                    continue;
                }
                if (s[j] == q) {
                    closed = true;
                    ++j;
                    break;
                }
                ++j;
            }
            const size_t end = closed ? j : n;
            bool key = false;
            if (lang == Lang::json && closed) {
                size_t k = j;
                while (k < n && is_space(s[k])) ++k;
                key = k < n && s[k] == ':';
            }
            w.add(s.substr(i, end - i), key ? st.keyword : st.string);
            i = end;
            continue;
        }
        if (sp.shell_quote && c == '$') {
            size_t j = i + 1;
            if (j < n && s[j] == '{') {
                const size_t p = s.find('}', j + 1);
                if (p != std::string_view::npos) {
                    w.add(s.substr(i, p + 1 - i), st.type);
                    i = p + 1;
                    continue;
                }
            }
            size_t k = j;
            while (k < n && is_ident_char(s[k])) ++k;
            if (k > j) {
                w.add(s.substr(i, k - i), st.type);
                i = k;
                continue;
            }
            w.add(s.substr(i, 1), st.text);
            ++i;
            continue;
        }
        if (c >= '0' && c <= '9') {
            size_t j = i;
            if (n - i >= 2 && s[i] == '0' &&
                (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                j = i + 2;
                while (j < n &&
                       (std::isxdigit(static_cast<unsigned char>(s[j])) ||
                        s[j] == '_')) {
                    ++j;
                }
            } else if (n - i >= 2 && s[i] == '0' &&
                       (s[i + 1] == 'b' || s[i + 1] == 'B')) {
                j = i + 2;
                while (j < n && (s[j] == '0' || s[j] == '1' || s[j] == '_')) {
                    ++j;
                }
            } else if (n - i >= 2 && s[i] == '0' &&
                       (s[i + 1] == 'o' || s[i + 1] == 'O')) {
                j = i + 2;
                while (j < n && ((s[j] >= '0' && s[j] <= '7') || s[j] == '_')) {
                    ++j;
                }
            } else {
                while (j < n &&
                       (std::isdigit(static_cast<unsigned char>(s[j])) ||
                        s[j] == '_')) {
                    ++j;
                }
                if (j < n && s[j] == '.' && !(j + 1 < n && s[j + 1] == '.')) {
                    ++j;
                    while (j < n &&
                           (std::isdigit(static_cast<unsigned char>(s[j])) ||
                            s[j] == '_')) {
                        ++j;
                    }
                }
                if (j < n && (s[j] == 'e' || s[j] == 'E')) {
                    size_t k = j + 1;
                    if (k < n && (s[k] == '+' || s[k] == '-')) ++k;
                    if (k < n &&
                        std::isdigit(static_cast<unsigned char>(s[k]))) {
                        j = k;
                        while (j < n &&
                               std::isdigit(static_cast<unsigned char>(s[j]))) {
                            ++j;
                        }
                    }
                }
            }
            w.add(s.substr(i, j - i), st.number);
            i = j;
            continue;
        }
        if (is_ident_start(c)) {
            size_t j = i;
            while (j < n && is_ident_char(s[j])) ++j;
            const std::string_view id = s.substr(i, j - i);
            Style style = st.text;
            if (is_keyword(lang, id)) {
                style = st.keyword;
            } else if (next_non_space(s, j) == '(') {
                style = st.function;
            } else if (id[0] >= 'A' && id[0] <= 'Z') {
                style = st.type;
            }
            w.add(id, style);
            i = j;
            continue;
        }
        w.add(s.substr(i, 1), st.text);
        ++i;
    }
}

} // namespace

size_t SyntaxRenderer::render(const Block& block, int width, const Theme& theme,
                              size_t from, size_t valid,
                              std::vector<Line>& out) const {
    const Lang lang = detect_lang(block.meta);
    const Spec sp = spec_of(lang);

    LexStyles st;
    st.text = theme.text;
    st.keyword = theme.text;
    st.keyword.attrs = st.keyword.attrs | Attr::bold;
    st.type = theme.text;
    st.type.attrs = st.type.attrs | Attr::italic;
    st.function = theme.text;
    st.function.attrs = st.function.attrs | Attr::underline;
    st.string = theme.code;
    st.comment = theme.dim;
    st.number = theme.text;

    const std::string_view src = block.source;
    const size_t limit = block.collapsed
                             ? std::min<size_t>(block.collapsed_rows, block.row_count)
                             : block.row_count;
    size_t n = valid;
    size_t pos = from;
    // 增量起点可能落在逻辑行中间（超宽折行的已定行）。词法按整条逻辑行
    // 进行，回退到行首、从该行的第一个物化行重画：代价 O(最后一行)，
    // 且行后部的内容（例如标识符后到达的 '('）能修正前面折行的着色。
    if (pos > 0 && pos < src.size()) {
        const size_t ls = line_start_of(src, pos);
        if (ls < pos) {
            while (n > 0 && out[n - 1].offset >= ls) --n;
            pos = ls;
        }
    }
    uint8_t state = (n > 0 && pos > 0) ? out[n - 1].lex : 0;
    thread_local RunCollector runs;
    std::string text;
    SpanWriter w;
    while (pos < src.size() && n < limit) {
        size_t le = pos;
        while (le < src.size() && src[le] != '\n' && src[le] != '\r') ++le;
        const std::string_view line = src.substr(pos, le - pos);
        const size_t ls = pos;
        runs.reset(line);
        if (lang != Lang::plain) lex_row(lang, sp, line, state, runs, st);
        const std::vector<LexRun>& rs = runs.runs();
        for (;;) {
            const RowEdge row = wrap_next_row(src, pos, width);
            Line& ln = ensure_line(out, n);
            w.reset(ln);
            if (lang == Lang::plain) {
                expand_row(text, src, pos, row.end);
                w.next(theme.code) = text;
            } else if (row.end > pos) {
                const size_t b = pos - ls;
                const size_t e = row.end - ls;
                int col = 0;
                auto r = std::upper_bound(
                    rs.begin(), rs.end(), b,
                    [](size_t v, const LexRun& x) { return v < x.at; });
                --r;
                for (; r != rs.end() && r->at < e; ++r) {
                    const size_t rb = std::max(b, r->at);
                    const size_t re = std::min(e, r + 1 != rs.end() ? (r + 1)->at
                                                                    : line.size());
                    append_expanded(w.next(r->style), line.substr(rb, re - rb), col);
                }
            }
            w.finish();
            ln.width = row.width;
            ln.offset = pos;
            ln.lex = state; // 整条逻辑行结束时的状态：续扫总从行首开始
            ++n;
            const bool crossed = row.next > row.end;
            pos = row.next;
            if (crossed || pos >= src.size() || n >= limit) break;
        }
    }
    return n;
}

} // namespace dagent::tui
