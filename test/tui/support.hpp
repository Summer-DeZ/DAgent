// 测试支撑：VT 屏幕回放器（差分等价性的判定基准）与分配计数。
#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "tui/grapheme.hpp"
#include "tui/surface.hpp"

// 断言失败时打印的值（经 ADL 被 Boost.Test 找到）。
namespace dagent::tui {

inline std::ostream& operator<<(std::ostream& os, const Size& s) {
    return os << "Size{" << s.cols << "," << s.rows << "}";
}
inline std::ostream& operator<<(std::ostream& os, const Point& p) {
    return os << "Point{" << p.x << "," << p.y << "}";
}
inline std::ostream& operator<<(std::ostream& os, const Rect& r) {
    return os << "Rect{" << r.x << "," << r.y << "," << r.w << "," << r.h << "}";
}
inline std::ostream& operator<<(std::ostream& os, const Color& c) {
    return os << "Color{" << int(c.kind) << ":" << int(c.r) << "," << int(c.g) << "," << int(c.b) << "}";
}
inline std::ostream& operator<<(std::ostream& os, const Style& s) {
    return os << "Style{fg=" << s.fg << " bg=" << s.bg << " attrs=" << int(s.attrs) << "}";
}

} // namespace dagent::tui

namespace test_support {

using namespace dagent::tui;

// ---- 分配计数：全局 operator new 在 main.cpp 中替换 ----

extern std::atomic<long> g_alloc_count;
extern thread_local bool g_alloc_counting;

class AllocScope {
public:
    AllocScope() noexcept {
        g_alloc_count = 0;
        g_alloc_counting = true;
    }
    ~AllocScope() { g_alloc_counting = false; }
    long allocations() const noexcept { return g_alloc_count.load(); }
};

// ---- VT 屏幕：按真实终端语义解释差分字节流 ----
//
// 只接受渲染器应当产生的序列（CUP、SGR、?25/?2026 私有模式），
// 其余任何序列或越界写入都记为错误 —— 回放结果与 back 逐格一致、
// 且无错误，才算差分正确。

struct VtCell {
    std::string text = " ";
    int width = 1;
    Style style{};
};

class VtScreen {
public:
    VtScreen(int cols, int rows) { resize(cols, rows); }

    // 模拟终端改变尺寸：重叠区域保留旧内容，新增区域为空白。
    void resize(int cols, int rows) {
        std::vector<VtCell> next(static_cast<std::size_t>(cols * rows));
        for (int r = 0; r < std::min(rows, rows_); ++r) {
            for (int c = 0; c < std::min(cols, cols_); ++c) {
                next[static_cast<std::size_t>(r * cols + c)] = cell(c, r);
            }
        }
        cells_ = std::move(next);
        cols_ = cols;
        rows_ = rows;
    }

    // 用非空白内容填满屏幕，模拟"终端上仍是旧帧"的状态。
    void scribble() {
        for (auto& c : cells_) {
            c = VtCell{"#", 1, Style{Color::indexed(5), Color{}, Attr::bold}};
        }
    }

    void feed(std::string_view bytes) {
        std::size_t i = 0;
        while (i < bytes.size()) {
            if (bytes[i] == '\x1b') {
                i = parse_escape(bytes, i);
                continue;
            }
            std::size_t j = bytes.find('\x1b', i);
            if (j == std::string_view::npos) j = bytes.size();
            print(bytes.substr(i, j - i));
            i = j;
        }
    }

    int cols() const noexcept { return cols_; }
    int rows() const noexcept { return rows_; }
    const VtCell& cell(int c, int r) const {
        return cells_[static_cast<std::size_t>(r * cols_ + c)];
    }
    const Style& pen() const noexcept { return pen_; }
    bool cursor_visible() const noexcept { return cursor_visible_; }
    int cursor_col() const noexcept { return col_; }
    int cursor_row() const noexcept { return row_; }
    int sync_depth() const noexcept { return sync_depth_; }
    const std::vector<std::string>& errors() const noexcept { return errors_; }

private:
    VtCell& at(int c, int r) { return cells_[static_cast<std::size_t>(r * cols_ + c)]; }

    std::size_t parse_escape(std::string_view b, std::size_t i) {
        if (i + 1 >= b.size() || b[i + 1] != '[') {
            errors_.push_back("non-CSI escape");
            return i + 1;
        }
        std::size_t j = i + 2;
        const bool priv = j < b.size() && b[j] == '?';
        if (priv) ++j;
        const std::size_t params_begin = j;
        while (j < b.size() && !(b[j] >= 0x40 && b[j] <= 0x7e)) ++j;
        if (j >= b.size()) {
            errors_.push_back("unterminated CSI");
            return b.size();
        }
        const std::vector<int> ps = split_params(b.substr(params_begin, j - params_begin));
        const char final = b[j];
        if (priv) {
            private_mode(ps, final);
        } else if (final == 'H') {
            row_ = (ps.size() > 0 ? ps[0] : 1) - 1;
            col_ = (ps.size() > 1 ? ps[1] : 1) - 1;
        } else if (final == 'm') {
            sgr(ps);
        } else {
            errors_.push_back(std::string("unexpected CSI final ") + final);
        }
        return j + 1;
    }

    static std::vector<int> split_params(std::string_view s) {
        std::vector<int> out;
        std::size_t k = 0;
        while (k <= s.size()) {
            std::size_t e = s.find(';', k);
            if (e == std::string_view::npos) e = s.size();
            int v = 0;
            std::from_chars(s.data() + k, s.data() + e, v);
            out.push_back(v);
            k = e + 1;
        }
        return out;
    }

    void private_mode(const std::vector<int>& ps, char final) {
        const bool set = final == 'h';
        if (final != 'h' && final != 'l') {
            errors_.push_back("unexpected private final");
            return;
        }
        for (int p : ps) {
            if (p == 25) {
                cursor_visible_ = set;
            } else if (p == 2026) {
                sync_depth_ += set ? 1 : -1;
            } else {
                errors_.push_back("unexpected private mode " + std::to_string(p));
            }
        }
    }

    void sgr(const std::vector<int>& ps) {
        for (std::size_t k = 0; k < ps.size(); ++k) {
            const int p = ps[k];
            switch (p) {
            case 0: pen_ = Style{}; break;
            case 1: pen_.attrs = pen_.attrs | Attr::bold; break;
            case 2: pen_.attrs = pen_.attrs | Attr::dim; break;
            case 3: pen_.attrs = pen_.attrs | Attr::italic; break;
            case 4: pen_.attrs = pen_.attrs | Attr::underline; break;
            case 5: pen_.attrs = pen_.attrs | Attr::blink; break;
            case 7: pen_.attrs = pen_.attrs | Attr::reverse; break;
            case 9: pen_.attrs = pen_.attrs | Attr::strike; break;
            case 22: pen_.attrs = pen_.attrs & ~(Attr::bold | Attr::dim); break;
            case 23: pen_.attrs = pen_.attrs & ~Attr::italic; break;
            case 24: pen_.attrs = pen_.attrs & ~Attr::underline; break;
            case 25: pen_.attrs = pen_.attrs & ~Attr::blink; break;
            case 27: pen_.attrs = pen_.attrs & ~Attr::reverse; break;
            case 29: pen_.attrs = pen_.attrs & ~Attr::strike; break;
            case 39: pen_.fg = Color{}; break;
            case 49: pen_.bg = Color{}; break;
            case 38:
            case 48: {
                Color c{};
                if (k + 2 < ps.size() && ps[k + 1] == 5) {
                    c = Color::indexed(static_cast<uint8_t>(ps[k + 2]));
                    k += 2;
                } else if (k + 4 < ps.size() && ps[k + 1] == 2) {
                    c = Color::rgb(static_cast<uint8_t>(ps[k + 2]),
                                   static_cast<uint8_t>(ps[k + 3]),
                                   static_cast<uint8_t>(ps[k + 4]));
                    k += 4;
                } else {
                    errors_.push_back("malformed extended color");
                    return;
                }
                (p == 38 ? pen_.fg : pen_.bg) = c;
                break;
            }
            default:
                errors_.push_back("unexpected SGR " + std::to_string(p));
            }
        }
    }

    void print(std::string_view text) {
        unicode::Grapheme g;
        while (unicode::next_grapheme(text, g)) {
            if (g.width <= 0) {
                errors_.push_back("zero-width grapheme emitted");
                continue;
            }
            if (row_ < 0 || row_ >= rows_ || col_ < 0 || col_ + g.width > cols_) {
                errors_.push_back("write outside screen");
                col_ += g.width;
                continue;
            }
            // 真实终端覆盖宽字符的一半时，另一半被抹成空白。
            VtCell& c = at(col_, row_);
            if (c.width == 0 && col_ > 0) at(col_ - 1, row_) = VtCell{};
            if (g.width == 1 && c.width == 2 && col_ + 1 < cols_) at(col_ + 1, row_) = VtCell{};
            if (g.width == 2) {
                VtCell& right = at(col_ + 1, row_);
                if (right.width == 2 && col_ + 2 < cols_) at(col_ + 2, row_) = VtCell{};
                right = VtCell{"", 0, pen_};
            }
            at(col_, row_) = VtCell{std::string(g.bytes), g.width, pen_};
            col_ += g.width;
        }
    }

    std::vector<VtCell> cells_;
    int cols_ = 0;
    int rows_ = 0;
    int col_ = 0;
    int row_ = 0;
    Style pen_{};
    bool cursor_visible_ = true;
    int sync_depth_ = 0;
    std::vector<std::string> errors_;
};

// 回放屏幕与 Surface 逐格比较；一致返回空串，否则描述第一个差异。
inline std::string screen_mismatch(const VtScreen& vt, const Surface& s) {
    if (vt.cols() != s.cols() || vt.rows() != s.rows()) return "size differs";
    for (int r = 0; r < s.rows(); ++r) {
        for (int c = 0; c < s.cols(); ++c) {
            const VtCell& v = vt.cell(c, r);
            const Cell& e = s.at(c, r);
            const bool same = v.width == e.width && v.style == e.style &&
                              (e.width == 0 || v.text == e.grapheme());
            if (!same) {
                return "cell (" + std::to_string(c) + "," + std::to_string(r) + "): vt '" +
                       v.text + "' w" + std::to_string(v.width) + ", surface '" +
                       std::string(e.grapheme()) + "' w" + std::to_string(e.width);
            }
        }
    }
    return {};
}

// 一行的可见文本（跳过宽字符右半占位格）。
inline std::string row_text(const Surface& s, int row) {
    std::string out;
    for (int c = 0; c < s.cols(); ++c) {
        const Cell& cell = s.at(c, row);
        if (cell.width != 0) out += cell.grapheme();
    }
    return out;
}

} // namespace test_support
