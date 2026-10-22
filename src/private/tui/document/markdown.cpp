#include "tui/document/document.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/document/markdown_detail.hpp"
#include "tui/grid/grapheme.hpp"

namespace dagent::tui {

namespace markdown_detail {

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

/// @brief 块级标记最多允许 3 个前导空格。
size_t leading_spaces(std::string_view s) noexcept {
    size_t i = 0;
    while (i < s.size() && i < 3 && s[i] == ' ') ++i;
    return i;
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

/// @brief 列表项标记前允许任意缩进。
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

/// @brief 表格分隔行：以 '|' 开头，单元只含 '-' ':' 空白，且至少一个 '-'。
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

LineClass classify(std::string_view line) noexcept {
    if (blank(line)) return LineClass::blank;
    if (hr(line)) return LineClass::hr;
    if (heading(line)) return LineClass::heading;
    if (list_item(line)) return LineClass::list;
    if (quote_line(line)) return LineClass::quote;
    if (table_sep(line)) return LineClass::table_sep;
    return LineClass::text;
}

} // namespace markdown_detail

using namespace markdown_detail;

namespace {

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

/// @brief 闭合围栏：同字符、长度不短于开启、其后只有空白。
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

/// @brief 代码块中未完成行仍可能是闭合围栏（只含围栏字符与空白）。
bool could_close_fence(std::string_view s, char ch) noexcept {
    const size_t i = leading_spaces(s);
    for (size_t j = i; j < s.size(); ++j) {
        if (s[j] == ch || is_space(s[j])) continue;
        return false;
    }
    return true;
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

/// @brief pending_ 增长但尚无换行：普通行立即建块显示，疑似围栏/表格开头的
/// 先攒到整行到达再判定。
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

/// @brief pending_ 是一整行；has_newline 表示行尾带 '\n'。
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
        // 空行不进块，只回退本行已显示的空白。
        if (!pending_.empty()) remove_last(pending_.size());
        close_current();
        return;
    }
    if (k == LineClass::hr) {
        if (has_newline) doc_->append(cur_, "\n");
        if (line_new_) {
            close_current(); // 分隔线自成一块，不回退
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
    // 列表/引用的续行留在当前块。
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

/// @brief 行不在任何块中（模式 none，或表格刚结束）：整行决定新块。
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

/// @brief 上边距：消息内第一块用 first_margin_，其后每块空 1 行。
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

} // namespace dagent::tui
