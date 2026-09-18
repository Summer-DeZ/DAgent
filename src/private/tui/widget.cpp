#include "tui/widget.hpp"

#include "tui/grapheme.hpp"

#include <algorithm>
#include <iterator>

namespace dagent::tui {

namespace {

constexpr int k_tab_stop = 8;

// 转圈符号（盲文点阵，窄字符）。
constexpr std::string_view k_spinner[10] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
};

// 单个字素的显示推进量：tab 展开、控制符跳过；返回 -1 = 行终止。
int advance(int col, const unicode::Grapheme& g) noexcept {
    const unsigned char b0 = static_cast<unsigned char>(g.bytes[0]);
    // CRLF 聚成一个簇：行终止看首字节。
    if (b0 == '\n' || b0 == '\r') return -1;
    if (g.bytes.size() == 1) {
        if (b0 == '\t') return (col / k_tab_stop + 1) * k_tab_stop;
        if (b0 < 0x20 || b0 == 0x7F) return col;
    }
    return col + g.width;
}

int line_width(std::string_view line) noexcept {
    int col = 0;
    unicode::Grapheme g;
    while (unicode::next_grapheme(line, g)) {
        const int next = advance(col, g);
        if (next < 0) break;
        col = next;
    }
    return col;
}

// 光标字节偏移 → 显示列。
int display_col_of(std::string_view line, int byte) noexcept {
    int col = 0;
    int pos = 0;
    unicode::Grapheme g;
    while (pos < byte && unicode::next_grapheme(line, g)) {
        const int next = advance(col, g);
        if (next < 0) break;
        col = next;
        pos += static_cast<int>(g.bytes.size());
    }
    return col;
}

// 显示列 → 字节偏移（落在字素边界上）。
int byte_of_display_col(std::string_view line, int col) noexcept {
    int c = 0;
    int pos = 0;
    unicode::Grapheme g;
    while (c < col && unicode::next_grapheme(line, g)) {
        const int next = advance(c, g);
        if (next < 0) break;
        c = next;
        pos += static_cast<int>(g.bytes.size());
    }
    return pos;
}

int prev_grapheme_start(std::string_view line, int byte) noexcept {
    int prev = 0;
    int pos = 0;
    unicode::Grapheme g;
    while (pos < byte && unicode::next_grapheme(line, g)) {
        prev = pos;
        pos += static_cast<int>(g.bytes.size());
    }
    return prev;
}

int next_grapheme_end(std::string_view line, int byte) noexcept {
    std::string_view rest = line;
    rest.remove_prefix(static_cast<std::size_t>(byte));
    unicode::Grapheme g;
    if (unicode::next_grapheme(rest, g)) {
        return byte + static_cast<int>(g.bytes.size());
    }
    return byte;
}

// 是否含需规范化的控制符。
bool needs_normalize(std::string_view s) noexcept {
    for (const char ch : s) {
        const unsigned char b = static_cast<unsigned char>(ch);
        if ((b < 0x20 && b != '\t' && b != '\n') || b == 0x7F) return true;
    }
    return false;
}

// \r\n 与单独的 \r 转为 \n，其余控制符丢弃。
std::string normalize_input(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const unsigned char b = static_cast<unsigned char>(in[i]);
        if (b == '\r') {
            out += '\n';
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
            continue;
        }
        if ((b < 0x20 && b != '\t' && b != '\n') || b == 0x7F) continue;
        out += static_cast<char>(b);
    }
    return out;
}

} // namespace

// ---- Text ----

void Text::set_text(std::string s) {
    text_ = std::move(s);
    resplit();
    invalidate_layout();
}

void Text::resplit() {
    lines_.clear();
    width_ = 0;
    std::size_t begin = 0;
    while (true) {
        const std::size_t nl = text_.find('\n', begin);
        const auto len = nl == std::string::npos ? text_.size() - begin
                                                 : nl - begin;
        const std::string_view part{text_.data() + begin, len};
        lines_.push_back(part);
        width_ = std::max(width_, line_width(part));
        if (nl == std::string::npos) break;
        begin = nl + 1;
    }
}

Size Text::measure(Size) const { return {width_, static_cast<int>(lines_.size())}; }

void Text::render(Surface& s) {
    s.fill({0, 0, s.cols(), s.rows()}, U' ', theme_->background);
    const int rows = std::min(static_cast<int>(lines_.size()), s.rows());
    for (int y = 0; y < rows; ++y) {
        s.text(0, y, lines_[static_cast<std::size_t>(y)], style_);
    }
}

// ---- Activity ----

void Activity::set_action(std::string text) {
    action_ = std::move(text);
    width_ = action_.empty() ? 0 : line_width(action_) + 2;
    invalidate_layout();
}

void Activity::tick() noexcept {
    frame_ = static_cast<uint8_t>((frame_ + 1) % std::size(k_spinner));
    invalidate();
}

Size Activity::measure(Size) const {
    return action_.empty() ? Size{} : Size{width_, 1};
}

void Activity::render(Surface& s) {
    s.fill({0, 0, s.cols(), s.rows()}, U' ', theme_->background);
    if (action_.empty()) return;
    s.put(0, 0, k_spinner[frame_], theme_->primary);
    s.text(2, 0, action_, theme_->text_muted);
}

// ---- Notice ----

Style Notice::style_of(Severity sev) const noexcept {
    switch (sev) {
    case Severity::warn:
        return theme_->warning;
    case Severity::error:
        return theme_->error;
    case Severity::info:
        break;
    }
    return theme_->info;
}

void Notice::show(Severity sev, std::string text) {
    sev_ = sev;
    text_ = std::move(text);
    width_ = text_.empty() ? 0 : line_width(text_);
    invalidate_layout();
}

Size Notice::measure(Size) const {
    return text_.empty() ? Size{} : Size{width_, 1};
}

void Notice::render(Surface& s) {
    s.fill({0, 0, s.cols(), s.rows()}, U' ', theme_->background);
    if (text_.empty()) return;
    s.text(0, 0, text_, style_of(sev_));
}

// ---- InputBox ----

int InputBox::caret_col() const noexcept {
    return display_col_of(lines_[static_cast<std::size_t>(line_)], col_);
}

void InputBox::set_text(std::string s) {
    if (needs_normalize(s)) s = normalize_input(s);
    lines_.clear();
    line_widths_.clear();
    std::size_t begin = 0;
    while (true) {
        const std::size_t nl = s.find('\n', begin);
        std::string part = nl == std::string::npos
                               ? s.substr(begin)
                               : s.substr(begin, nl - begin);
        line_widths_.push_back(line_width(part));
        lines_.push_back(std::move(part));
        if (nl == std::string::npos) break;
        begin = nl + 1;
    }
    line_ = 0;
    col_ = 0;
    vscroll_ = 0;
    hscroll_ = 0;
    caret_moved();
    invalidate_layout();
}

std::string InputBox::text() const {
    std::string out;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines_[i];
    }
    return out;
}

void InputBox::insert(std::string_view utf8) {
    std::string normalized;
    if (needs_normalize(utf8)) {
        normalized = normalize_input(utf8);
        utf8 = normalized;
    }
    if (utf8.empty()) return;

    if (utf8.find('\n') == std::string_view::npos) {
        std::string& cur = lines_[static_cast<std::size_t>(line_)];
        cur.insert(static_cast<std::size_t>(col_), utf8);
        line_widths_[static_cast<std::size_t>(line_)] = line_width(cur);
        col_ += static_cast<int>(utf8.size());
        caret_moved();
        invalidate_layout();
        return;
    }

    const int suffix =
        static_cast<int>(lines_[static_cast<std::size_t>(line_)].size()) - col_;
    std::string merged = lines_[static_cast<std::size_t>(line_)];
    merged.insert(static_cast<std::size_t>(col_), utf8);

    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (true) {
        const std::size_t nl = merged.find('\n', begin);
        if (nl == std::string::npos) {
            parts.emplace_back(merged, begin);
            break;
        }
        parts.emplace_back(merged, begin, nl - begin);
        begin = nl + 1;
    }

    std::vector<int> ws;
    ws.reserve(parts.size() - 1);
    for (auto it = parts.begin() + 1; it != parts.end(); ++it) {
        ws.push_back(line_width(*it));
    }

    // 尾节含原光标后的后缀，光标停在插入内容之后。
    const int tail = static_cast<int>(parts.back().size()) - suffix;
    lines_[static_cast<std::size_t>(line_)] = std::move(parts[0]);
    line_widths_[static_cast<std::size_t>(line_)] =
        line_width(lines_[static_cast<std::size_t>(line_)]);
    lines_.insert(lines_.begin() + line_ + 1,
                  std::make_move_iterator(parts.begin() + 1),
                  std::make_move_iterator(parts.end()));
    line_widths_.insert(line_widths_.begin() + line_ + 1, ws.begin(), ws.end());
    line_ += static_cast<int>(ws.size());
    col_ = tail;
    caret_moved();
    invalidate_layout();
}

void InputBox::backspace() {
    const std::size_t li = static_cast<std::size_t>(line_);
    if (col_ > 0) {
        const int prev = prev_grapheme_start(lines_[li], col_);
        lines_[li].erase(static_cast<std::size_t>(prev),
                         static_cast<std::size_t>(col_ - prev));
        line_widths_[li] = line_width(lines_[li]);
        col_ = prev;
    } else if (line_ > 0) {
        // 行首退格：并入上一行，光标停在接缝处。
        const int seam = static_cast<int>(lines_[li - 1].size());
        lines_[li - 1] += lines_[li];
        lines_.erase(lines_.begin() + line_);
        line_widths_[li - 1] = line_width(lines_[li - 1]);
        line_widths_.erase(line_widths_.begin() + line_);
        --line_;
        col_ = seam;
    } else {
        return; // 首行行首：无处可退
    }
    caret_moved();
    invalidate_layout();
}

void InputBox::del() {
    const std::size_t li = static_cast<std::size_t>(line_);
    if (col_ < static_cast<int>(lines_[li].size())) {
        const int end = next_grapheme_end(lines_[li], col_);
        lines_[li].erase(static_cast<std::size_t>(col_),
                         static_cast<std::size_t>(end - col_));
        line_widths_[li] = line_width(lines_[li]);
    } else if (line_ + 1 < static_cast<int>(lines_.size())) {
        // 行尾 delete：下一行并入本行，光标不动。
        lines_[li] += lines_[li + 1];
        lines_.erase(lines_.begin() + line_ + 1);
        line_widths_[li] = line_width(lines_[li]);
        line_widths_.erase(line_widths_.begin() + line_ + 1);
    } else {
        return; // 末行行尾：无处可删
    }
    caret_moved();
    invalidate_layout();
}

void InputBox::move(int dcols, int dlines) {
    if (dcols == 0 && dlines == 0) return;
    const int n = static_cast<int>(lines_.size());

    if (dlines != 0) {
        if (goal_ < 0) goal_ = caret_col();
        line_ = std::clamp(line_ + dlines, 0, n - 1);
        col_ = byte_of_display_col(lines_[static_cast<std::size_t>(line_)], goal_);
    }

    if (dcols > 0) {
        while (dcols > 0) {
            const std::string& cur = lines_[static_cast<std::size_t>(line_)];
            if (col_ < static_cast<int>(cur.size())) {
                col_ = next_grapheme_end(cur, col_);
            } else if (line_ + 1 < n) {
                ++line_;
                col_ = 0;
            } else {
                break;
            }
            --dcols;
        }
        caret_moved();
    } else if (dcols < 0) {
        while (dcols < 0) {
            if (col_ > 0) {
                col_ = prev_grapheme_start(lines_[static_cast<std::size_t>(line_)], col_);
            } else if (line_ > 0) {
                --line_;
                col_ = static_cast<int>(lines_[static_cast<std::size_t>(line_)].size());
            } else {
                break;
            }
            ++dcols;
        }
        caret_moved();
    }
    invalidate();
}

void InputBox::line_home() {
    col_ = 0;
    caret_moved();
    invalidate();
}

void InputBox::line_end() {
    col_ = static_cast<int>(lines_[static_cast<std::size_t>(line_)].size());
    caret_moved();
    invalidate();
}

std::optional<Point> InputBox::cursor() const {
    const int iw = rect().w - 2; // 边框内净宽/净高
    const int ih = rect().h - 2;
    if (iw <= 0 || ih <= 0) return std::nullopt;
    const int row = line_ - vscroll_;
    const int col = caret_col() - hscroll_;
    if (row < 0 || row >= ih || col < 0 || col >= iw) return std::nullopt;
    return Point{col + 1, row + 1};
}

Size InputBox::measure(Size) const {
    int w = 0;
    for (int lw : line_widths_) w = std::max(w, lw);
    return {w + 2, static_cast<int>(lines_.size()) + 2};
}

/// @brief 边框 + 可见窗口；滚动只在这里推进。
void InputBox::render(Surface& s) {
    const int w = s.cols();
    const int h = s.rows();
    s.fill({0, 0, w, h}, U' ', theme_->background);
    if (w < 3 || h < 3) return; // 放不下边框：只剩清底

    const Style edge = theme_->border;
    s.fill({0, 0, w, 1}, U'─', edge);
    s.fill({0, h - 1, w, 1}, U'─', edge);
    s.fill({0, 0, 1, h}, U'│', edge);
    s.fill({w - 1, 0, 1, h}, U'│', edge);
    s.put(0, 0, "┌", edge);
    s.put(w - 1, 0, "┐", edge);
    s.put(0, h - 1, "└", edge);
    s.put(w - 1, h - 1, "┘", edge);

    const int ih = h - 2;
    const int iw = w - 2;
    const int n = static_cast<int>(lines_.size());

    if (line_ < vscroll_) vscroll_ = line_;
    if (line_ >= vscroll_ + ih) vscroll_ = line_ - ih + 1;
    vscroll_ = std::clamp(vscroll_, 0, std::max(0, n - ih));

    const int cc = caret_col();
    if (cc < hscroll_) hscroll_ = cc;
    if (cc >= hscroll_ + iw) hscroll_ = cc - iw + 1;
    if (hscroll_ < 0) hscroll_ = 0;

    const int last = std::min(n, vscroll_ + ih);
    Surface inner = s.view({1, 1, iw, ih}); // 内容裁剪在边框内
    // 整行从 -hscroll_ 起写，裁剪由 Surface::text 处理。
    for (int i = vscroll_; i < last; ++i) {
        inner.text(-hscroll_, i - vscroll_, lines_[static_cast<std::size_t>(i)],
                   theme_->text);
    }
}

} // namespace dagent::tui
