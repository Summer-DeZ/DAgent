/// @file document.hpp
/// @brief 大内容滚动区：块模型、折行缓存与 Scrollback 控件。
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

#include "tui/widget.hpp"

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
/// @brief 对全文折行计数（零分配）。
size_t count_rows(std::string_view source, int width) noexcept;

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

/// @brief 文本渲染器：每行一段，样式取指定的 ThemeTokens 槽位。
class TextRenderer final : public BlockRenderer {
public:
    explicit TextRenderer(Style ThemeTokens::*slot = &ThemeTokens::text) noexcept
        : slot_(slot) {}

    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;

private:
    Style ThemeTokens::*slot_;
};

/// @brief 差异渲染器：'+' / '-' / '@' 开头的行用 diff_* 令牌，其余正文。
class DiffRenderer final : public BlockRenderer {
public:
    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;
};

/// @brief Markdown 渲染器：隐藏行内标记，按显示文本折行。
class MarkdownRenderer final : public BlockRenderer {
public:
    WrapResult measure(std::string_view source, size_t from,
                       int width) const override;
    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;
};

/// @brief 表格渲染器：列宽依赖全部行，每次变化整块重排。
class TableRenderer final : public BlockRenderer {
public:
    WrapResult measure(std::string_view source, size_t from,
                       int width) const override;
    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;
};

/// @brief 语法高亮渲染器：语言取自 Block::meta 的首个词，左侧留 k_gutter 列。
class SyntaxRenderer final : public BlockRenderer {
public:
    static constexpr int k_gutter = 2;

    WrapResult measure(std::string_view source, size_t from, int width) const override;
    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;
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
    /// @brief 建一个可增长的块（流式输出入口），返回块 id。
    uint64_t open_block(BlockKind kind, std::string source = {});
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
    /// @brief 头部裁剪到至多 keep 块。
    void trim_blocks(size_t keep);
    /// @brief 头部裁剪到至多 keep 行。
    void trim_rows(size_t keep);

    /// @brief 滚动锚点；replace/erase_from 自动维护其有效性。
    const Anchor& anchor() const noexcept { return anchor_; }
    void set_anchor(Anchor anchor) noexcept { anchor_ = anchor; }

    size_t block_count() const noexcept { return blocks_.size(); }
    const Block& block_at(size_t index) const noexcept { return blocks_[index]; }
    /// @brief 按 id 查找；不存在返回 nullptr。
    const Block* find(uint64_t id) const noexcept;
    /// @brief 内容版本号，任何内容变更后递增。
    uint64_t revision() const noexcept { return revision_; }

    // ---- 帧路径（渲染线程） ----

    /// @brief 开帧：宽度或主题纪元变化时全量重数，否则只增量重扫置脏块。
    void begin_frame(int width, uint32_t theme_epoch);
    /// @brief 可见总行数（0 = 现存最旧一行）。
    size_t total_rows() const noexcept {
        return prefix_.back() - base_rows_;
    }
    /// @brief 锚点所在行；所在块被头部裁剪后返回 nullopt。
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
    std::deque<size_t> prefix_; ///< 长度 = blocks_ + 1，绝对行号
    std::array<std::unique_ptr<BlockRenderer>, k_block_kind_count> renderers_{};
    Anchor anchor_{};
    uint64_t next_id_ = 1;
    uint64_t revision_ = 0;
    size_t first_dirty_ = static_cast<size_t>(-1);
    size_t base_rows_ = 0; ///< 已裁剪的行数（绝对行号 = 可见行号 + 它）
    int width_ = 0;
    uint32_t theme_epoch_ = 0;
};

/// @brief 把增量 Markdown 原文切成 Document 块，只有最后一个块增长。
class MarkdownStream {
public:
    /// @param first_margin 每条消息第一块的上边距（消息内其余块为 1）。
    explicit MarkdownStream(Document& doc, uint8_t first_margin = 0) noexcept
        : doc_(&doc), first_margin_(first_margin) {}
    MarkdownStream(const MarkdownStream&) = delete;
    MarkdownStream& operator=(const MarkdownStream&) = delete;

    /// @brief 追加任意分块的原文。
    void feed(std::string_view chunk);
    /// @brief 消息结束：未完成的行保留在块内，关闭最后一个块。
    void finish();

private:
    enum class Mode : uint8_t { none, markdown, code, table };
    enum class Family : uint8_t { text, heading, list, quote };

    void on_partial();              ///< pending_ 增长（尚无换行）
    void on_line(bool has_newline); ///< pending_ 是一整行
    void handle_markdown_line(std::string_view line, bool has_newline);
    void handle_detached_line(std::string_view line, bool has_newline,
                              bool allow_table);
    void open_markdown(std::string source, bool pipe);
    void open_table(std::string source);
    void start_code(char fence, size_t len, std::string info);
    Block new_block(BlockKind kind); ///< 按消息内位置设置上边距
    void close_current();
    void remove_last(size_t bytes);

    Document* doc_;
    Mode mode_ = Mode::none;
    Family family_ = Family::text;
    uint64_t cur_ = 0;            ///< 当前 open 块
    char fence_ch_ = 0;           ///< 代码围栏字符
    size_t fence_len_ = 0;        ///< 围栏长度
    bool code_candidate_ = false; ///< 未完成行可能是闭合围栏
    bool line_in_block_ = false;  ///< 当前行已（部分）写入当前块
    bool line_new_ = false;       ///< 当前行的块是行中途新建的
    bool last_line_pipe_ = false; ///< 当前 markdown 块最后一行以 '|' 开头
    size_t last_line_len_ = 0;    ///< 该行字节数（含换行）
    size_t sent_ = 0;             ///< pending_ 中已追加的字节数
    std::string pending_;         ///< 未完成行（不含换行）
    std::string staged_;          ///< 暂存的表格候选表头（含换行）
    bool staging_ = false;
    bool started_ = false; ///< 本条消息已建过块
    uint8_t first_margin_ = 0;
};

/// @brief 大内容滚动区：Document 的门面 widget。
class Scrollback : public Widget {
public:
    Document& document() noexcept { return doc_; }
    const Document& document() const noexcept { return doc_; }

    /// @brief 换主题（更改样式后需递增 ThemeTokens::epoch）。
    void set_theme(ThemeTokens theme) {
        theme_ = std::move(theme);
        invalidate();
    }
    const ThemeTokens& theme() const noexcept { return theme_; }

    // ---- 滚动（正数 = 向下/新内容方向） ----
    void scroll_lines(int lines);
    void scroll_pages(int pages);
    void scroll_home();
    void scroll_end(); ///< 贴底并保持跟随

    [[nodiscard]] bool pinned() const noexcept {
        return doc_.anchor().pinned_to_bottom;
    }
    /// @brief 视口下方的行数（不贴底时 > 0）。
    [[nodiscard]] size_t unseen_rows() const noexcept { return unseen_; }

    // ---- 选择 ----

    /// @brief 视图坐标 → 内容位置；文档为空时返回 nullopt。
    std::optional<Location> hit(Point p);
    void select(Selection s);
    void clear_selection();
    [[nodiscard]] const std::optional<Selection>& selection() const noexcept {
        return selection_;
    }
    /// @brief 选区的源文本；无选区时为空串。
    [[nodiscard]] std::string selected_text() const;

    Size measure(Size available) const override;
    void render(Surface& s) override;
    bool dirty_tree() const noexcept override;

private:
    void draw_row(Surface& s, int y, size_t row, const Line& ln);
    size_t max_top() const noexcept;
    /// @brief 把视口顶行锚到绝对行 row；dir < 0 表示向上滚动。
    void anchor_to(size_t row, int dir);

    Document doc_;
    ThemeTokens theme_ = dark_theme();
    uint64_t rendered_rev_ = 0; ///< 已渲染到的内容版本
    size_t total_ = 0;          ///< 上次渲染的文档总行数
    size_t top_ = 0;            ///< 上次渲染的视口顶行
    size_t unseen_ = 0;         ///< 视口下方行数
    int view_ = 0;              ///< 上次渲染的视口高度
    std::optional<Selection> selection_;
};

} // namespace dagent::tui
