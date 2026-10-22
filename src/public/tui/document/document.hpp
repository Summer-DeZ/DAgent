/// @file document.hpp
/// @brief 大内容滚动区的文档模型：块、锚点与选区、折行接口、渲染器基类与 Document。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tui/widget/widget.hpp"

namespace dagent::tui {

/// @brief 块的内容形态。
enum class BlockKind : uint8_t { text, code, diff, output, markdown, table };
inline constexpr std::size_t k_block_kind_count = 6;

/// @brief Span::src 的哨兵值：无源装饰。
inline constexpr size_t k_no_src = static_cast<size_t>(-1);

/// @brief 物化行的一段连续同样式文本，可直接交给 Surface::text。
struct Span {
    std::string text; ///< 制表符已展开为空格
    Style style{};
    size_t src = k_no_src; ///< 首字节在 source 中的偏移；k_no_src = 装饰
};

/// @brief 物化行（屏幕行）。
struct Line {
    std::vector<Span> spans;
    int width = 0;     ///< 显示宽度（列）
    size_t offset = 0; ///< 行首在 source 中的字节偏移
    bool reordered = false; ///< 源片段顺序不同于显示行序时，锚点按 span 查找
    uint8_t lex = 0;   ///< 逻辑行末的跨行词法状态（仅语法渲染器解释）
};

/// @brief 逻辑块：source 是逻辑原文，未折行、未上色。
struct Block {
    // ---- 逻辑内容 ----
    uint64_t id = 0;        ///< 单调递增，永不复用
    BlockKind kind = BlockKind::text;
    std::string source;
    std::string meta;       ///< 附属信息（如代码语言）
    uint32_t group = 0;     ///< 分组
    uint8_t depth = 0;      ///< 缩进层级
    uint8_t margin_top = 0; ///< 块前空行数（块无内容时不生效）
    bool open = false;      ///< 仍在增长
    bool collapsed = false;
    uint32_t collapsed_rows = 0; ///< 折叠后最多显示的行数

    // ---- 派生缓存（Document 维护，调用方只读） ----
    int64_t cache_key = -1; ///< 宽度 + 折叠状态 + 主题纪元的打包键
    size_t row_count = 0;   ///< 全程行数
    size_t stable_rows = 0; ///< 断点已定的行数
    size_t stable_bytes = 0; ///< stable_rows 的起始字节
    size_t rows_valid = 0;   ///< rows 中有效的前缀行数
    size_t rows_bytes = 0;   ///< rows_valid 覆盖到的 source 字节数
    std::vector<Line> rows;  ///< 物化结果
};

/// @brief 滚动锚点：贴底跟随新内容，否则停在块内字节偏移处。
struct Anchor {
    uint64_t block_id = 0;
    size_t byte_in_block = 0;
    bool pinned_to_bottom = true;
};

/// @brief 内容位置：块 + 块内字节偏移。
struct Location {
    uint64_t block_id = 0;
    size_t byte_in_block = 0;
    auto operator<=>(const Location&) const = default;
};

/// @brief 选区：两个内容位置（闭区间）。
struct Selection {
    Location anchor;
    Location head;
};

/// @brief 折行计数结果。
struct WrapResult {
    size_t rows = 0;         ///< 显示行数（含结尾不完整行）
    size_t stable_rows = 0;  ///< 断点已定的行数
    size_t stable_bytes = 0; ///< stable_rows 的下一行起点（相对计数起点）
};

/// @brief 从 source[from] 起折行计数（零分配）；from 必须是行边界，
/// 返回值中的 stable_bytes 相对 from。
WrapResult wrap_measure_from(std::string_view source, size_t from,
                             int width) noexcept;

/// @brief 单行折行边界。
struct RowEdge {
    size_t end = 0;   ///< 本行内容为 [from, end)
    size_t next = 0;  ///< 下一行起始字节
    int width = 0;    ///< 本行显示宽度
    size_t cut = 0;   ///< 超宽断行位置的字素起点
};
/// @brief 从 source[from] 求下一行的折行边界。
RowEdge wrap_next_row(std::string_view source, size_t from, int width) noexcept;

/// @brief 把 source[begin, end) 展开为可绘制文本写入 dst（清空、保留容量）。
void expand_row(std::string& dst, std::string_view source, size_t begin,
                size_t end);

/// @brief 块渲染器接口。
class BlockRenderer {
public:
    virtual ~BlockRenderer() = default;

    /// @brief 只数行，不分配字符串。
    /// @note 无法保证已定前缀稳定的实现应返回 stable_rows = stable_bytes = 0。
    virtual WrapResult measure(std::string_view source, size_t from, int width) const {
        return wrap_measure_from(source, from, width);
    }

    /// @brief 折行并上色，覆盖写入 out[valid...]；返回有效行数。
    /// @param valid out 的 [0, valid) 前缀仍有效
    virtual size_t render(const Block& block, int width, const ThemeTokens& theme,
                          size_t from, size_t valid,
                          std::vector<Line>& out) const = 0;
};

/// @brief 文档：块序列 + 行数前缀和 + 折行缓存。
class Document {
public:
    Document();

    // ---- 内容变更（渲染线程；只改内存并置脏） ----

    /// @brief 追加一个块，返回块 id。
    uint64_t append_block(BlockKind kind, std::string source = {});
    /// @brief 追加块（完整控制 meta/group/depth/open/collapsed），返回块 id。
    uint64_t append_block(Block block);
    /// @brief 追加到 open 块；id 不存在或块已关闭时返回 false。
    bool append(uint64_t id, std::string_view chunk);
    /// @brief 整体替换块的 source。
    bool replace(uint64_t id, std::string source);
    /// @brief 改 meta 并使该块物化失效。
    bool set_meta(uint64_t id, std::string meta);
    /// @brief 删除 id 及其后所有块，返回删除数。
    size_t erase_from(uint64_t id);
    /// @brief 终结增长。
    bool close_block(uint64_t id);
    /// @brief 折叠/展开；折叠后最多显示 rows 行。
    bool set_collapsed(uint64_t id, bool collapsed, uint32_t rows);

    /// @brief 清空文档；id 序列保持单调不复用。
    void clear();

    /// @brief 滚动锚点；replace/erase_from 自动维护其有效性。
    const Anchor& anchor() const noexcept { return anchor_; }
    void set_anchor(Anchor anchor) noexcept { anchor_ = anchor; }

    /// @brief 按 id 查找；不存在返回 nullptr。
    const Block* find(uint64_t id) const noexcept;
    /// @brief 内容版本号，任何内容变更后递增。
    uint64_t revision() const noexcept { return revision_; }

    // ---- 帧路径（渲染线程） ----

    /// @brief 开帧：宽度或主题纪元变化时全量重数，否则只增量重扫置脏块。
    void begin_frame(int width, uint32_t theme_epoch);
    /// @brief 可见总行数（0 = 现存最旧一行）。
    size_t total_rows() const noexcept {
        return prefix_.back();
    }
    /// @brief 锚点所在行；所在块被删除后返回 nullopt。
    std::optional<size_t> row_of(uint64_t block_id, size_t byte_in_block,
                                 const ThemeTokens& theme);
    /// @brief 行号 → 内容位置；边距行返回块首。
    Location location_of(size_t row, const ThemeTokens& theme);
    /// @brief 物化与 [first, first+count) 相交的块。
    void materialize_range(size_t first, size_t count, const ThemeTokens& theme);
    /// @brief 取行；未物化返回 nullptr，边距行返回空行。
    const Line* line_at(size_t row) const noexcept;
    /// @brief 驱逐可见窗口 ± count 行以外块的 rows（open 块除外）。
    void evict_outside(size_t first, size_t count) noexcept;

    /// @brief 块内容（不含上边距）是否与行窗口相交；使用最近 begin_frame 的计数。
    bool block_visible(uint64_t id, size_t first, size_t count) const noexcept;

    // ---- 选择 ----

    /// @brief 屏幕行列 → 内容位置。
    Location location_at(size_t row, int col, const ThemeTokens& theme);
    /// @brief [a, b] 之间的源文本；任一端块不存在时返回空串。
    std::string text_between(Location a, Location b) const;
    /// @brief 双击选词（不在词上时只选该字素）。
    Selection word_around(Location loc) const;
    /// @brief 三击选逻辑行（不含行尾换行）。
    Selection line_around(Location loc) const;

    // ---- 渲染器注册（扩展点） ----

    /// @brief 替换某形态的渲染器，已有物化块整体置脏。
    void set_renderer(BlockKind kind, std::unique_ptr<BlockRenderer> renderer);
    const BlockRenderer& renderer(BlockKind kind) const noexcept;

private:
    void count_full(Block& b);
    void count_incremental(Block& b);
    void rebuild_prefix(size_t from);
    void ensure_rows(Block& b, const ThemeTokens& theme);
    std::optional<size_t> index_of(uint64_t id) const noexcept;

    std::deque<Block> blocks_;
    std::deque<size_t> prefix_; ///< 长度 = blocks_ + 1，累计行号
    std::array<std::unique_ptr<BlockRenderer>, k_block_kind_count> renderers_{};
    Anchor anchor_{};
    uint64_t next_id_ = 1;
    uint64_t revision_ = 0;
    size_t first_dirty_ = static_cast<size_t>(-1);
    int width_ = 0;
    uint32_t theme_epoch_ = 0;
};


} // namespace dagent::tui
