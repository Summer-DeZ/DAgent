#include "tui/widget.hpp"

#include "tui/grapheme.hpp"

#include <algorithm>
#include <iterator>

namespace dagent::tui {

namespace {

constexpr int k_tab_stop = 8;

// 转圈符号（盲文点阵，窄字符）。动画帧节奏由 L7 驱动。
constexpr std::string_view k_spinner[10] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
};

// 单个字素的显示推进量。与 Surface::text 的宽度规则完全一致：
// 制表符展开到 tab stop，'\n'/'\r' 终止（返回 -1 作信号），其余
// 控制符跳过，零宽簇不计宽。
int advance(int col, const unicode::Grapheme& g) noexcept {
    if (g.bytes.size() == 1) {
        const unsigned char b = static_cast<unsigned char>(g.bytes[0]);
        if (b == '\t') return (col / k_tab_stop + 1) * k_tab_stop;
        if (b == '\n' || b == '\r') return -1;
        if (b < 0x20 || b == 0x7F) return col;
    }
    return col + g.width;
}

// 整行显示宽度（渲染宽度的权威定义，与 Surface::text 逐格一致）。
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

// 光标字节偏移 → 显示列（只走到光标为止）。
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

// 显示列 → 字节偏移：第一个起始列 >= col 的字素之前。
// 光标永远落在字素边界上，不会停在宽字符两列之间。
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

// 光标前一个字素簇的起始字节。行短（输入框尺度），O(col) 从头走。
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

// 光标后一个字素簇的结束字节（不越出行尾）。
int next_grapheme_end(std::string_view line, int byte) noexcept {
    std::string_view rest = line;
    rest.remove_prefix(static_cast<std::size_t>(byte));
    unicode::Grapheme g;
    if (unicode::next_grapheme(rest, g)) {
        return byte + static_cast<int>(g.bytes.size());
    }
    return byte;
}

// 编辑模型的内容不变量：只含 '\n' 分行、'\t' 与可见字符。
// '\r' 会截断渲染、其余控制符不可见却占光标步，因此在入口处规范化。
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

// 行切片 + 宽度缓存。行数与最长行宽度只在这里算一次，
// measure/render 都是 O(1)/O(行数) 的缓存读取。
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
    s.fill({0, 0, s.cols(), s.rows()}, U' ', Style{});
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
    s.fill({0, 0, s.cols(), s.rows()}, U' ', Style{});
    if (action_.empty()) return;
    s.put(0, 0, k_spinner[frame_], Style{});
    s.text(2, 0, action_, Style{});
}

// ---- Notice ----

Style Notice::style_of(Severity sev) noexcept {
    switch (sev) {
    case Severity::warn:
        return Style{Color::indexed(3), Color{}, Attr::none};
    case Severity::error:
        return Style{Color::indexed(1), Color{}, Attr::bold};
    case Severity::info:
        break;
    }
    return Style{};
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
    s.fill({0, 0, s.cols(), s.rows()}, U' ', Style{});
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

// 插入 '\n' 时把当前行重分节：首节留在原行，其余节成为新行插到后面，
// 光标落在最后一个插入字素之后（行内容 = 尾节去掉原光标之后的后缀）。
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

    // 拷贝后再分节：视图不受 vector 扩容 / string 重分配影响。
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

    // 尾节 = 插入的末段 + 原光标之后的后缀；光标停在插入内容之后。
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

// 相对移动。垂直先走（使用/确立目标显示列），水平后走（跨行），
// 水平移动后以新位置重置目标列 —— 连续上下移动保持原始列。
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

// 边框 + 可见窗口。滚动只在这里推进（渲染线程），每次都把光标
// 夹回可见窗口：行数/列数随编辑变化时不需要额外的滚动维护逻辑。
void InputBox::render(Surface& s) {
    const int w = s.cols();
    const int h = s.rows();
    s.fill({0, 0, w, h}, U' ', Style{});
    if (w < 3 || h < 3) return; // 放不下边框：只剩清底

    const Style edge{};
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
    Surface inner = s.view({1, 1, iw, ih}); // 内容裁剪在边框内，结构性保证
    // 整行从 -hscroll_ 起写：制表符与宽字符按真实显示列展开，
    // 左侧裁剪与跨边界宽字符的半格由 Surface::text 处理。
    for (int i = vscroll_; i < last; ++i) {
        inner.text(-hscroll_, i - vscroll_, lines_[static_cast<std::size_t>(i)], Style{});
    }
}

} // namespace dagent::tui
