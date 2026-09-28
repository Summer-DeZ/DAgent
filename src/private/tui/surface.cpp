#include "tui/surface.hpp"

#include "tui/grapheme.hpp"

#include <atomic>
#include <cstring>
#include <deque>
#include <unordered_map>

namespace dagent::tui {

namespace {

constexpr Cell k_blank_cell{};

// 超长字素 intern 表：进程级、渲染线程专用。
std::deque<std::string> g_interned;
std::unordered_map<std::string_view, uint32_t> g_intern_lookup;
std::atomic<bool> g_intern_overflow{false};

uint32_t intern(std::string_view g) {
    if (auto it = g_intern_lookup.find(g); it != g_intern_lookup.end()) {
        return it->second;
    }
    g_interned.emplace_back(g);
    const auto id = static_cast<uint32_t>(g_interned.size() - 1);
    g_intern_lookup.emplace(g_interned.back(), id);
    if (g_interned.size() > k_intern_max) {
        g_intern_overflow.store(true, std::memory_order_relaxed);
    }
    return id;
}

Cell make_cell(std::string_view g, int width, const Style& s) noexcept {
    Cell c{};
    if (g.size() <= 4) {
        for (std::size_t i = 0; i < g.size(); ++i) c.text[i] = g[i];
    } else {
        // 0xFF 是合法 UTF-8 首字节不可能的值，天然充当 intern 标记。
        const uint32_t id = intern(g);
        c.text[0] = static_cast<char>(0xFF);
        c.text[1] = static_cast<char>((id >> 16) & 0xFF);
        c.text[2] = static_cast<char>((id >> 8) & 0xFF);
        c.text[3] = static_cast<char>(id & 0xFF);
    }
    c.width = static_cast<uint8_t>(width);
    c.style = s;
    return c;
}

} // namespace

std::string_view Cell::grapheme() const noexcept {
    if (static_cast<unsigned char>(text[0]) == 0xFF) {
        const uint32_t id = (static_cast<uint32_t>(static_cast<unsigned char>(text[1])) << 16) |
                            (static_cast<uint32_t>(static_cast<unsigned char>(text[2])) << 8) |
                            static_cast<uint32_t>(static_cast<unsigned char>(text[3]));
        return g_interned[id];
    }
    std::size_t n = 0;
    while (n < 4 && text[n] != '\0') ++n;
    return {text, n};
}

void Surface::resize(int cols, int rows) {
    if (!owner_ || (cols == cols_ && rows == rows_ && cells_ != nullptr)) {
        return;
    }
    cols_ = cols;
    rows_ = rows;
    stride_ = cols;
    if (cols <= 0 || rows <= 0) {
        storage_.clear();
        dirty_storage_.clear();
        cells_ = nullptr;
        row_dirty_ = nullptr;
        return;
    }
    // 新内容为空白，全部置脏触发整屏重绘。
    storage_.assign(static_cast<std::size_t>(cols) * rows, k_blank_cell);
    dirty_storage_.assign(static_cast<std::size_t>(rows), 1);
    cells_ = storage_.data();
    row_dirty_ = dirty_storage_.data();
}

void Surface::copy_from(const Surface& src) noexcept {
    if (!owner_ || cells_ == nullptr || src.cells_ == nullptr) {
        return;
    }
    if (src.cols_ != cols_ || src.rows_ != rows_) {
        return; // 尺寸错位：present() 会走全量重绘路径
    }
    std::memcpy(cells_, src.cells_,
                static_cast<std::size_t>(cols_) * rows_ * sizeof(Cell));
    clear_dirty();
}

void Surface::clear() noexcept {
    fill({0, 0, cols_, rows_}, U' ', Style{});
}

void Surface::clear_dirty() noexcept {
    if (row_dirty_ != nullptr) {
        std::memset(row_dirty_, 0, static_cast<std::size_t>(rows_));
    }
}

bool Surface::row_dirty(int row) const noexcept {
    return row >= 0 && row < rows_ && row_dirty_ != nullptr && row_dirty_[row] != 0;
}

const Cell& Surface::at(int col, int row) const noexcept {
    if (col < 0 || row < 0 || col >= cols_ || row >= rows_ || cells_ == nullptr) {
        return k_blank_cell;
    }
    return cells_[static_cast<std::size_t>(row) * stride_ + col];
}

// 单格写入并维护宽字符两半一致：被覆盖的半格修复为空格，新宽字右邻写占位格。
// 调用方保证 col/row 在界内且宽字放得下。
void Surface::write_cell(int col, int row, std::string_view g, int w,
                         const Style& s) noexcept {
    Cell& c = cells_[static_cast<std::size_t>(row) * stride_ + col];
    if (c.width == 0) {
        if (col > 0 || x0_ > 0) {
            cells_[static_cast<std::size_t>(row) * stride_ + col - 1] =
                make_cell(" ", 1, s);
        }
    }
    if (w == 2) {
        Cell& right = cells_[static_cast<std::size_t>(row) * stride_ + col + 1];
        if (right.width == 2) {
            const int orphan = col + 2;
            if (orphan < cols_ || x0_ + orphan < stride_) {
                cells_[static_cast<std::size_t>(row) * stride_ + orphan] =
                    make_cell(" ", 1, s);
            }
        }
        c = make_cell(g, 2, s);
        right = c;
        right.width = 0;
    } else {
        if (c.width == 2) {
            const int orphan = col + 1;
            if (orphan < cols_ || x0_ + orphan < stride_) {
                cells_[static_cast<std::size_t>(row) * stride_ + orphan] =
                    make_cell(" ", 1, s);
            }
        }
        c = make_cell(g, 1, s);
    }
    row_dirty_[row] = 1;
}

void Surface::put(int col, int row, std::string_view g, const Style& s) noexcept {
    if (col < 0 || row < 0 || col >= cols_ || row >= rows_ || g.empty()) {
        return;
    }
    std::string_view tmp = g;
    unicode::Grapheme gr;
    const int w = unicode::next_grapheme(tmp, gr) ? gr.width : 1;
    if (w <= 0) {
        return; // 零宽簇不留格
    }
    if (w == 2 && col + 1 >= cols_) {
        // 宽字在视图右缘放不下：降级写空格。
        write_cell(col, row, " ", 1, s);
        return;
    }
    write_cell(col, row, g, w, s);
}

// 文本写入：逐字素直接走 write_cell；write_cell 不做边界裁剪，负列在此挡住。
int Surface::text(int col, int row, std::string_view s, const Style& st,
                  int tab_stop) noexcept {
    if (row < 0 || row >= rows_ || tab_stop <= 0) {
        return col;
    }
    const int origin = col;
    unicode::Grapheme g;
    while (col < cols_ && unicode::next_grapheme(s, g)) {
        const unsigned char b0 = static_cast<unsigned char>(g.bytes[0]);
        // CRLF 聚成一个簇：终止判断只看首字节。
        if (b0 == '\n' || b0 == '\r') {
            break;
        }
        if (g.bytes.size() == 1) {
            const unsigned char b = b0;
            if (b == '\t') {
                // 制表符写入时展开成空格；tab stop 相对文本起点，负列只推进不写入。
                int stop = origin + ((col - origin) / tab_stop + 1) * tab_stop;
                if (stop > cols_) stop = cols_;
                while (col < stop) {
                    if (col >= 0) {
                        write_cell(col, row, " ", 1, st);
                    }
                    ++col;
                }
                continue;
            }
            if (b < 0x20 || b == 0x7F) {
                continue; // 其余控制符丢弃
            }
        }
        if (g.width <= 0) {
            continue; // 零宽独立簇（格式字符等）不留格
        }
        if (g.width == 2 && col + 1 >= cols_) {
            break; // 宽字放不下：整簇停止，不做半格
        }
        if (col < 0) {
            // 负列只推进不写入；宽字跨过第 0 列时给可见半格补空格。
            if (g.width == 2 && col + 1 == 0) {
                write_cell(0, row, " ", 1, st);
            }
            col += g.width;
            continue;
        }
        write_cell(col, row, g.bytes, g.width, st);
        col += g.width;
    }
    return col;
}

// 区域填充：按字素宽度步进，右缘放不下的宽字降级为空格。
void Surface::fill(Rect r, char32_t ch, const Style& st) noexcept {
    if (unicode::char_width(ch) == 0) {
        return;
    }
    r = r.intersect({0, 0, cols_, rows_});
    if (r.empty()) {
        return;
    }
    char enc[4];
    const std::size_t n = unicode::encode_utf8(ch, enc);
    const std::string_view g(enc, n);
    const int w = unicode::char_width(ch) == 2 ? 2 : 1;
    for (int row = r.y; row < r.bottom(); ++row) {
        int col = r.x;
        while (col < r.right()) {
            if (w == 2 && col + 1 >= r.right()) {
                write_cell(col, row, " ", 1, st);
                ++col;
            } else {
                write_cell(col, row, g, w, st);
                col += w;
            }
        }
    }
}

Surface Surface::view(Rect r) noexcept {
    r = r.intersect({0, 0, cols_, rows_});
    Surface v;
    v.owner_ = false;
    v.cols_ = r.w;
    v.rows_ = r.h;
    v.stride_ = stride_;
    v.x0_ = x0_ + r.x; // 宿主坐标系列原点，跨边界半格修复时使用
    if (cells_ != nullptr && v.cols_ > 0 && v.rows_ > 0) {
        v.cells_ = cells_ + static_cast<std::size_t>(r.y) * stride_ + r.x;
        v.row_dirty_ = row_dirty_ + r.y; // 脏标记写穿到宿主对应行
    }
    return v;
}

bool intern_overflowed() noexcept {
    return g_intern_overflow.load(std::memory_order_relaxed);
}

void intern_reset() noexcept {
    g_intern_lookup.clear();
    g_interned.clear();
    g_intern_overflow.store(false, std::memory_order_relaxed);
}

} // namespace dagent::tui
