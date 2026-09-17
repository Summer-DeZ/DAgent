// L2 表面：单元格网格 + 双缓冲 + 损伤差分 —— 整个框架性能的地基。
// 屏幕被建模为单元格网格而不是行字符串：
//   * 相邻/重叠 widget 的输出可以正确合成，字符串只能整行替换；
//   * 样式跨行由差分器统一补发 SGR；
//   * 宽字符占两列、组合字符占零列在 Cell 里显式表达，"第 N 列"是 O(1) 查询；
//   * 差分能做到行内区间粒度，而不是整行粒度。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tui/terminal.hpp"

namespace dagent::tui {

struct Point {
    int x = 0;
    int y = 0;
    bool operator==(const Point&) const noexcept = default;
};

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool operator==(const Rect&) const noexcept = default;

    constexpr int right() const noexcept { return x + w; }
    constexpr int bottom() const noexcept { return y + h; }
    constexpr bool empty() const noexcept { return w <= 0 || h <= 0; }
    constexpr Rect intersect(Rect o) const noexcept {
        const int x0 = x > o.x ? x : o.x;
        const int y0 = y > o.y ? y : o.y;
        const int x1 = right() < o.right() ? right() : o.right();
        const int y1 = bottom() < o.bottom() ? bottom() : o.bottom();
        return {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
    }
};

// 颜色：1 字节 tag + 3 字节值，恰好 4 字节；indexed 时 r 即调色板索引。
struct Color {
    enum class Kind : uint8_t { default_, indexed, rgb };
    Kind kind = Kind::default_;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    bool operator==(const Color&) const noexcept = default;

    static constexpr Color indexed(uint8_t i) noexcept {
        return {Kind::indexed, i, 0, 0};
    }
    static constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) noexcept {
        return {Kind::rgb, r, g, b};
    }
};

// 文本属性位。帧末统一 \e[0m 归零，帧首终端恒处于默认态，
// 差分器用 Style{} 即可表达起点，无需哨兵。
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

struct Style {
    Color fg{};
    Color bg{};
    Attr attrs = Attr::none;
    bool operator==(const Style&) const noexcept = default;
};

// 单个单元格：绝大多数字素是单码点、≤4 字节 UTF-8，直接内联在 text 里；
// 超长簇（emoji ZWJ 序列等）text[0] = 0xFF（UTF-8 首字节不可能的值），
// 其余 3 字节是 intern 表索引。width: 0 = 宽字符右半占位 / 1 = 半角 / 2 = 全角。
// 16 字节（含 1 字节对齐填充）；用默认 operator== 逐成员比较，填充不参与。
// 300×100 双缓冲约 1MB。
struct Cell {
    char text[4]{' ', 0, 0, 0};
    uint8_t width = 1;
    Style style{};
    bool operator==(const Cell&) const noexcept = default;

    // 还原字素字节；intern 簇由表还原（渲染线程专用，无锁）。
    std::string_view grapheme() const noexcept;
};

// 表面。owner 持有存储；view() 返回的视图与宿主共享存储（span 语义）：
// 坐标系从 0 开始、写入被裁剪在区域内 —— "widget 画不进别人区域"是结构性保证。
// 视图在宿主 resize 后失效，纪律同 string_view；视图中跨边界的宽字符
// 由写入时的右缘规则保证不产生半格残留。
class Surface {
public:
    Surface() = default;
    Surface(int cols, int rows) { resize(cols, rows); }
    // cells_ 指向自身 storage_：拷贝会指回源缓冲区，禁止；移动时 vector
    // 缓冲区地址不变，指针仍然有效。
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    Surface(Surface&&) noexcept = default;
    Surface& operator=(Surface&&) noexcept = default;

    void resize(int cols, int rows); // 仅 owner；尺寸不变时不做任何事
    void clear() noexcept;           // 全部填空格 + 默认样式，并整体置脏
    // 帧起点：back ← front（一次 memcpy）。这让"未失效区域 = 终端真相"，
    // 失效 widget 只需重画自己的矩形。尺寸错位时为空操作（由 present 处理）。
    void copy_from(const Surface& src) noexcept;
    void clear_dirty() noexcept;

    int cols() const noexcept { return cols_; }
    int rows() const noexcept { return rows_; }
    bool row_dirty(int row) const noexcept;
    const Cell& at(int col, int row) const noexcept; // 越界返回静态空格

    // 绘制原语：坐标越界自动裁剪，不 UB、不抛异常（仅 OOM 可终止）。
    void put(int col, int row, std::string_view grapheme, const Style& s) noexcept;
    // 写一段文本，按字素推进列，返回写到的列；制表符就地展开到 tab stop
    // （相对起点 col），控制符跳过，\n/\r 终止写入，宽字符放不下时整簇停止。
    // col 可为负：左侧被裁掉的部分只推进不写入（用于水平滚动）。
    int text(int col, int row, std::string_view utf8, const Style& s,
             int tab_stop = 8) noexcept;
    void fill(Rect r, char32_t ch, const Style& s) noexcept;
    void hline(int row, int col0, int col1, const Style& s) noexcept; // U+2500

    Surface view(Rect r) noexcept;

private:
    void write_cell(int col, int row, std::string_view g, int w,
                    const Style& s) noexcept;

    std::vector<Cell> storage_;      // 仅 owner 持有
    std::vector<uint8_t> dirty_storage_;
    Cell* cells_ = nullptr;          // 视图指向宿主存储的子区域
    uint8_t* row_dirty_ = nullptr;   // 指向宿主脏标记数组的行偏移
    int cols_ = 0;
    int rows_ = 0;
    int stride_ = 0;                 // 宿主宽度；owner 时 == cols_
    int x0_ = 0;                     // 视图在宿主中的列原点；owner 时为 0
    bool owner_ = true;
};

// 帧选项：能力降级开关 + 光标落点（无焦点时保持隐藏）+ 全量重绘开关。
struct FrameOptions {
    bool synchronized = false;
    bool truecolor = true;
    std::optional<Point> cursor;
    // 强制逐行整行写出、忽略行脏标记。用于 resize：终端真实状态还是
    // 旧尺寸的旧内容，若按差分走，back 中的空格会被判成"没变化"，
    // 旧字符就此残留。调用方（L7）在 resize 纪元还需 invalidate_tree()
    // 保证 back 内容完整。
    bool full_repaint = false;
};

// 渲染是纯函数：(back, front, 选项) → 差分字节。无副作用、可重放、可断言。
// 正常路径四个关键优化：行级脏标记跳过、行内差分区间、SGR 游程增量合并、
// DEC 2026 同步帧。front 尺寸错位或 full_repaint 时走全量路径（逐行整行）。
// 稳态路径 out 复用容量，零分配。
void render_frame(std::string& out, const Surface& back, const Surface& front,
                  const FrameOptions& opt);

// 组装 + 写出 + 双缓冲交换 + 清脏。全框架唯一写终端的路径，渲染线程调用。
// 尺寸错位时把 front 重置为空白并强制全量重绘 —— 终端上仍是旧尺寸的旧
// 内容，只有逐行整行写出才能保证写出后终端 == back。
void present(Terminal& term, Surface& back, Surface& front, std::string& out,
             std::optional<Point> cursor = std::nullopt);

} // namespace dagent::tui
