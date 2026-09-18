/// @file surface.hpp
/// @brief 单元格网格 + 双缓冲 + 损伤差分渲染。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tui/terminal.hpp"

namespace dagent::tui {

/// @brief 屏幕坐标（0 基）。
struct Point {
    int x = 0;
    int y = 0;
    bool operator==(const Point&) const noexcept = default;
};

/// @brief 屏幕矩形。
struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool operator==(const Rect&) const noexcept = default;

    constexpr int right() const noexcept { return x + w; }
    constexpr int bottom() const noexcept { return y + h; }
    constexpr bool empty() const noexcept { return w <= 0 || h <= 0; }
    constexpr bool contains(Point p) const noexcept {
        return p.x >= x && p.y >= y && p.x < right() && p.y < bottom();
    }
    constexpr Rect intersect(Rect o) const noexcept {
        const int x0 = x > o.x ? x : o.x;
        const int y0 = y > o.y ? y : o.y;
        const int x1 = right() < o.right() ? right() : o.right();
        const int y1 = bottom() < o.bottom() ? bottom() : o.bottom();
        return {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
    }
};

// 颜色定义见 terminal.hpp。

/// @brief 文本属性位。
enum class Attr : uint16_t {
    none      = 0,
    bold      = 1 << 0,
    dim       = 1 << 1,
    italic    = 1 << 2,
    underline = 1 << 3,
    blink     = 1 << 4,
    reverse   = 1 << 5,
    strike    = 1 << 6,
};

constexpr Attr operator|(Attr a, Attr b) noexcept {
    return static_cast<Attr>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
constexpr Attr operator&(Attr a, Attr b) noexcept {
    return static_cast<Attr>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}
constexpr Attr operator~(Attr a) noexcept {
    return static_cast<Attr>(~static_cast<uint16_t>(a));
}
constexpr bool any(Attr a) noexcept { return static_cast<uint16_t>(a) != 0; }

/// @brief 前景/背景色 + 属性位。
struct Style {
    Color fg{};
    Color bg{};
    Attr attrs = Attr::none;
    bool operator==(const Style&) const noexcept = default;
};

/// @brief 单元格。
struct Cell {
    /// UTF-8 字节；超长簇时 text[0] = 0xFF，其余 3 字节为 intern 表索引。
    char text[4]{' ', 0, 0, 0};
    uint8_t width = 1; ///< 0 = 宽字符右半占位 / 1 = 半角 / 2 = 全角
    Style style{};
    bool operator==(const Cell&) const noexcept = default;

    /// @brief 还原字素字节（intern 簇查表）。
    std::string_view grapheme() const noexcept;
};

/// @brief 表面：拥有存储的 owner，或共享存储的子视图（view()）。
class Surface {
public:
    Surface() = default;
    Surface(int cols, int rows) { resize(cols, rows); }
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    Surface(Surface&&) noexcept = default;
    Surface& operator=(Surface&&) noexcept = default;

    void resize(int cols, int rows); ///< 仅 owner；尺寸不变时不做任何事
    void clear() noexcept;           ///< 全部填空格并整体置脏
    /// @brief 帧起点：back ← front（尺寸错位时为空操作）。
    void copy_from(const Surface& src) noexcept;
    void clear_dirty() noexcept;

    int cols() const noexcept { return cols_; }
    int rows() const noexcept { return rows_; }
    bool row_dirty(int row) const noexcept;
    const Cell& at(int col, int row) const noexcept; ///< 越界返回静态空格

    /// @brief 写一个字素；越界自动裁剪。
    void put(int col, int row, std::string_view grapheme, const Style& s) noexcept;
    /// @brief 写一段文本，按字素推进列，返回写到的列；\n/\r 终止，控制符跳过。
    /// @note 制表符展开到相对起点的 tab stop；宽字符放不下时停止；
    /// col 可为负（左侧裁掉的部分只推进不写入）。
    int text(int col, int row, std::string_view utf8, const Style& s,
             int tab_stop = 8) noexcept;
    /// @brief 用 ch 填充区域；宽字符在右缘放不下时降级为空格。
    void fill(Rect r, char32_t ch, const Style& s) noexcept;
    void hline(int row, int col0, int col1, const Style& s) noexcept; ///< U+2500 横线

    /// @brief 返回共享本表面存储的子视图（裁剪到宿主范围）。
    Surface view(Rect r) noexcept;

private:
    /// @brief 写单元格并维护宽字符两半的一致性。
    void write_cell(int col, int row, std::string_view g, int w,
                    const Style& s) noexcept;

    std::vector<Cell> storage_;    ///< 仅 owner 持有
    std::vector<uint8_t> dirty_storage_;
    Cell* cells_ = nullptr;        ///< 视图指向宿主存储的子区域
    uint8_t* row_dirty_ = nullptr; ///< 视图指向宿主脏标记的行偏移
    int cols_ = 0;
    int rows_ = 0;
    int stride_ = 0;               ///< 宿主宽度
    int x0_ = 0;                   ///< 视图在宿主中的列原点
    bool owner_ = true;
};

/// @brief 帧选项。
struct FrameOptions {
    bool synchronized = false;
    bool truecolor = true;
    std::optional<Point> cursor; ///< 光标落点；空 = 保持隐藏
    bool full_repaint = false;   ///< true 时忽略行脏标记，逐行整行写出
};

/// @brief 生成 back 与 front 的差分字节，追加到 out。
void render_frame(std::string& out, const Surface& back, const Surface& front,
                  const FrameOptions& opt);

/// @brief 组装帧、写出、交换双缓冲并清脏（唯一的终端写路径）。
void present(Terminal& term, Surface& back, Surface& front, std::string& out,
             std::optional<Point> cursor = std::nullopt);

/// @brief 超长字素 intern 表的容量上限。
inline constexpr std::size_t k_intern_max = 4096;
bool intern_overflowed() noexcept; ///< 表项是否超过上限
void intern_reset() noexcept;      ///< 清空 intern 表
std::size_t intern_size() noexcept; ///< 当前表项数

} // namespace dagent::tui
