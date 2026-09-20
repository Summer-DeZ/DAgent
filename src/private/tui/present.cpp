#include "tui/surface.hpp"

#include <charconv>
#include <algorithm>
#include <cstdlib>

namespace dagent::tui {

namespace {

void append_uint(std::string& out, int v) {
    char buf[16];
    const auto res = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, res.ptr);
}

/// @brief 绝对定位 CUP（1 基）。
void append_cup(std::string& out, int row1, int col1) {
    out += "\x1b[";
    append_uint(out, row1);
    out += ';';
    append_uint(out, col1);
    out += 'H';
}

/// @brief 无 truecolor 时把 RGB 量化到 xterm 256 色。
int quantize256(const Color& c) noexcept {
    // xterm 色立方并非均匀分布；近灰色也必须与灰阶比较，避免暗底被抬亮。
    constexpr int levels[] = {0, 95, 135, 175, 215, 255};
    const auto nearest = [&](int value) {
        int best = 0;
        for (int i = 1; i < 6; ++i)
            if (std::abs(value - levels[i]) < std::abs(value - levels[best])) best = i;
        return best;
    };
    const auto square = [](int value) { return value * value; };
    const int r = nearest(c.r), g = nearest(c.g), b = nearest(c.b);
    const int cube_error = square(c.r - levels[r]) + square(c.g - levels[g]) + square(c.b - levels[b]);
    const int gray = std::clamp((static_cast<int>(c.r) + c.g + c.b - 24 + 15) / 30, 0, 23);
    const int value = 8 + 10 * gray;
    const int gray_error = square(c.r - value) + square(c.g - value) + square(c.b - value);
    return gray_error < cube_error ? 232 + gray : 16 + 36 * r + 6 * g + b;
}

void append_color(std::string& out, const Color& c, bool foreground,
                  bool truecolor) {
    constexpr Color default_{};
    if (c == default_) {
        out += foreground ? "\x1b[39m" : "\x1b[49m";
        return;
    }
    const bool use256 = c.kind != Color::Kind::rgb || !truecolor;
    if (use256) {
        const int idx = c.kind == Color::Kind::rgb ? quantize256(c) : c.r;
        out += foreground ? "\x1b[38;5;" : "\x1b[48;5;";
        append_uint(out, idx);
    } else {
        out += foreground ? "\x1b[38;2;" : "\x1b[48;2;";
        append_uint(out, c.r);
        out += ';';
        append_uint(out, c.g);
        out += ';';
        append_uint(out, c.b);
    }
    out += 'm';
}

/// @brief SGR 增量：只发真正变化的属性；bold/dim 共用关闭码 22。
void append_sgr(std::string& out, const Style& from, const Style& to,
                bool truecolor) {
    const Attr dropped = from.attrs & ~to.attrs;
    const Attr added = to.attrs & ~from.attrs;

    if (any(dropped & (Attr::bold | Attr::dim))) {
        out += "\x1b[22m";
        if (any(to.attrs & Attr::bold)) out += "\x1b[1m";
        if (any(to.attrs & Attr::dim)) out += "\x1b[2m";
    } else {
        if (any(added & Attr::bold)) out += "\x1b[1m";
        if (any(added & Attr::dim)) out += "\x1b[2m";
    }

    if (any(dropped & Attr::italic)) out += "\x1b[23m";
    if (any(dropped & Attr::underline)) out += "\x1b[24m";
    if (any(dropped & Attr::blink)) out += "\x1b[25m";
    if (any(dropped & Attr::reverse)) out += "\x1b[27m";
    if (any(dropped & Attr::strike)) out += "\x1b[29m";

    if (any(added & Attr::italic)) out += "\x1b[3m";
    if (any(added & Attr::underline)) out += "\x1b[4m";
    if (any(added & Attr::blink)) out += "\x1b[5m";
    if (any(added & Attr::reverse)) out += "\x1b[7m";
    if (any(added & Attr::strike)) out += "\x1b[9m";

    if (!(to.fg == from.fg)) append_color(out, to.fg, true, truecolor);
    if (!(to.bg == from.bg)) append_color(out, to.bg, false, truecolor);
}

} // namespace

void render_frame(std::string& out, const Surface& back, const Surface& front,
                  const FrameOptions& opt) {
    out.clear(); // 保留容量
    if (opt.synchronized) out += "\x1b[?2026h"; // 开始同步帧
    out += "\x1b[?25l";                         // 绘制期间隐藏光标

    // 帧首样式处于默认态
    Style current{};
    const int cols = back.cols();
    const int rows = back.rows();
    // 尺寸错位或强制全量时逐行整行写出
    const bool full =
        opt.full_repaint || front.cols() != cols || front.rows() != rows;

    for (int row = 0; row < rows; ++row) {
        int first = 0;
        int last = cols - 1;
        if (!full) {
            if (!back.row_dirty(row)) {
                continue;
            }
            first = -1;
            last = -1;
            for (int col = 0; col < cols; ++col) {
                const bool same =
                    back.at(col, row) == front.at(col, row);
                if (!same) {
                    if (first < 0) first = col;
                    last = col;
                }
            }
            if (first < 0) {
                continue;
            }
        }
        append_cup(out, row + 1, first + 1);
        for (int col = first; col <= last; ++col) {
            const Cell& c = back.at(col, row);
            if (c.width == 0) {
                continue; // 宽字符右半不单独输出
            }
            if (!(c.style == current)) {
                append_sgr(out, current, c.style, opt.truecolor);
                current = c.style;
            }
            out += c.grapheme();
        }
    }

    // 帧末归零样式
    if (!(current == Style{})) out += "\x1b[0m";
    if (opt.cursor) {
        append_cup(out, opt.cursor->y + 1, opt.cursor->x + 1);
        out += "\x1b[?25h"; // 帧末定位并显示光标
    }
    if (opt.synchronized) out += "\x1b[?2026l";
}

void present(Terminal& term, Surface& back, Surface& front, std::string& out,
             std::optional<Point> cursor) {
    FrameOptions opt{term.caps().synchronized, term.caps().truecolor, cursor,
                     false};
    if (back.cols() != front.cols() || back.rows() != front.rows()) {
        // front 重置为空白网格，强制全量重写
        front.resize(back.cols(), back.rows());
        opt.full_repaint = true;
    }
    render_frame(out, back, front, opt);
    term.write(out);
    std::swap(front, back);
    back.clear_dirty();
}

} // namespace dagent::tui
