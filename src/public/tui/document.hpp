// L5 文档层：大内容滚动区（文档§八）。
//
// 内容模型是「块」而不是屏幕行：source 是唯一真相，rows 只是按当前宽度
// 推导出的派生缓存。由此得到三项结构保证：
//   * 改变宽度能重折、折叠/展开能切换形态、裁剪按块整块进行；
//   * 计数（对所有块、零分配）与物化（只对可见块）分离，代价差一个数量级；
//   * 流式块追加只做 source += chunk，折行在渲染时从 stable_bytes 增量
//     进行，代价是 O(最后一行) 而不是 O(全文)。
//
// 定位用「前缀和 + 二分」：尾部追加 O(1)，定位 O(log n)，头部裁剪 O(1)。
// 滚动位置用 Anchor（块 id + 块内行号）而不是「距底部 N 行」表示，
// 重折、裁剪、追加都不会让用户正在看的内容跳走。
//
// 本层唯一的扩展点是 BlockRenderer 注册表：新增内容形态（富文本、表格、
// 语法高亮…）只需实现渲染器并注册，不触碰框架其他部分（§8.6）。
//
// 线程纪律：可变状态（块的派生缓存、rows）不加锁。L7 用同一个 state
// mutex 串行化「业务线程修改内容 + 渲染线程折行/物化」（§十），
// 因此这里只做纯内存操作：变更侧置脏，渲染侧推导。
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

// 块的内容形态。默认注册表：text/code/output → 文本渲染（主题槽位不同），
// diff → 差异渲染。
enum class BlockKind : uint8_t { text, code, diff, output };
inline constexpr std::size_t k_block_kind_count = 4;

// 物化行的一段连续同样式文本。制表符已展开为空格（§5.4：网格里不存 '\t'，
// 否则每次列计算都要回溯），可直接交给 Surface::text。
struct Span {
    std::string text;
    Style style{};
};

// 物化行（屏幕行）。spans 支持一行内多段样式；增量物化会原地复用 Line
// 对象（覆盖写入保留 string 容量），流式帧路径因此不产生新分配。
struct Line {
    std::vector<Span> spans;
    int width = 0; // 显示宽度（列）
};

// 逻辑块（§8.1）。source 是逻辑原文，未折行、未上色。
struct Block {
    // ---- 逻辑内容 ----
    uint64_t id = 0;          // 单调递增，永不复用；锚点引用它
    BlockKind kind = BlockKind::text;
    std::string source;
    std::string meta;         // 标题、来源等附属信息（默认渲染器不解释）
    uint32_t group = 0;       // 分组（例如并发子任务）
    uint8_t depth = 0;        // 缩进层级（默认渲染器不解释，留给自定义渲染器）
    bool open = false;        // 仍在增长（只有尾部块适合，见 append）
    bool collapsed = false;
    uint32_t collapsed_rows = 0; // 折叠后最多显示的行数

    // ---- 派生缓存（Document 维护，调用方只读） ----
    int64_t cache_key = -1;   // 宽度 + 折叠状态 + 主题纪元 的打包键
    size_t row_count = 0;     // 全程行数（未折叠时的计数级结果）
    size_t stable_rows = 0;   // [0, stable_rows) 行的断点已定（§8.5）
    size_t stable_bytes = 0;  // stable_rows 行的起始字节，即计数级折行锚点
    size_t rows_valid = 0;    // rows 中与当前 source 一致的前缀行数
    size_t rows_bytes = 0;    // rows_valid 前缀覆盖到的 source 字节数（物化锚点）
    std::vector<Line> rows;   // 物化结果；被驱逐/失效时 rows_valid 归零
};

// 主题：默认渲染器用到的样式槽 + 纪元。任何样式字段变更后必须递增 epoch，
// 否则块的物化缓存（cache_key 含 epoch）不会失效。
struct Theme {
    Style text{};
    Style dim{};
    Style code{};
    Style add{}; // diff '+'
    Style del{}; // diff '-'
    uint32_t epoch = 0;
};

// 滚动锚点（§8.4）。贴底是独立状态而不是「偏移量为 0」：新内容到达时
// 贴底则跟随，不贴底则保持锚点不动。
struct Anchor {
    uint64_t block_id = 0;
    uint32_t row_in_block = 0;
    bool pinned_to_bottom = true;
};

// 块内定位结果（绝对行号由 Document::row_of 换算）。
struct Location {
    uint64_t block_id = 0;
    size_t row_in_block = 0;
};

// 折行计数结果（§8.2/§8.5）。除最后一行外，每一行的断点都由已有字符
// 唯一决定：stable_rows/stable_bytes 就是「已定」前缀的边界，
// 追加内容只需从 stable_bytes 重扫最后一行。
struct WrapResult {
    size_t rows = 0;         // 显示行数（含结尾不完整行）
    size_t stable_rows = 0;  // 断点已定的行数
    size_t stable_bytes = 0; // 相对计数起点的偏移：stable_rows 的下一行起点
};

// 只数行、零分配（§8.2 的计数级接口）。
WrapResult wrap_measure(std::string_view source, int width) noexcept;
// 从 source[from] 起继续计数；from 必须是行边界（前一行的起始字节），
// 返回值里的 stable_bytes 相对 from。Document 的增量折行锚点即由此推进。
WrapResult wrap_measure_from(std::string_view source, size_t from,
                             int width) noexcept;
size_t count_rows(std::string_view source, int width) noexcept;

// 块渲染器：把 source 变成带样式的行（§8.6）。实现必须满足
// count(s, w) == render(block(s), w, ...) 产出的行数（§十三.1 的等价性）。
class BlockRenderer {
public:
    virtual ~BlockRenderer() = default;

    // 只数行、不分配任何字符串。
    virtual size_t count(std::string_view source, int width) const = 0;

    // 从 source[from] 起折行/上色，覆盖写入 out[valid...]；out 的
    // [0, valid) 前缀由调用方保证仍然有效。返回写入后的有效行数。
    // 折叠块只产出 min(collapsed_rows, row_count) 行。
    virtual size_t render(const Block& block, int width, const Theme& theme,
                          size_t from, size_t valid,
                          std::vector<Line>& out) const = 0;
};

// 文本渲染器：每行一段，样式取自 Theme 的指定槽位（成员指针）。
class TextRenderer final : public BlockRenderer {
public:
    explicit TextRenderer(Style Theme::*slot = &Theme::text) noexcept
        : slot_(slot) {}

    size_t count(std::string_view source, int width) const override;
    size_t render(const Block& block, int width, const Theme& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;

private:
    Style Theme::*slot_;
};

// 差异渲染器：'+' / '-' / '@' 开头的行分别用 add/del/dim 槽位，其余正文。
class DiffRenderer final : public BlockRenderer {
public:
    size_t count(std::string_view source, int width) const override;
    size_t render(const Block& block, int width, const Theme& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;
};

// 文档：块序列 + 行数前缀和 + 两级折行缓存。
//
// 帧路径的调用顺序（见 Scrollback::render）：
//   begin_frame(w, theme.epoch);      // 宽度/主题校验 + 增量计数
//   materialize_range(top, h, theme); // 只物化可见窗口
//   line_at(row)                      // 取行绘制
//   evict_outside(top, h);            // 驱逐窗口 ± 一屏以外的缓存
class Document {
public:
    Document();

    // ---- 内容变更（业务线程）：只改内存并置脏，绝不 I/O ----
    uint64_t append_block(BlockKind kind, std::string source = {});
    uint64_t append_block(Block block); // 完整控制 meta/group/depth/open/collapsed
    // 建一个可增长的尾部块（流式输出的入口，配合 append/close_block）。
    uint64_t open_block(BlockKind kind, std::string source = {});
    // 追加到 open 块；只 source += chunk（O(chunk)，不折行、零分配）。
    // id 不存在或块已关闭时返回 false。
    bool append(uint64_t id, std::string_view chunk);
    bool close_block(uint64_t id); // 终结增长：行数从此不再变
    bool set_collapsed(uint64_t id, bool collapsed, uint32_t rows);

    void clear();
    void trim_blocks(size_t keep); // 头部裁剪到至多 keep 块，O(1)/块
    void trim_rows(size_t keep);   // 头部裁剪到至多 keep 行，O(1)/块

    size_t block_count() const noexcept { return blocks_.size(); }
    const Block& block_at(size_t index) const noexcept { return blocks_[index]; }
    const Block* find(uint64_t id) const noexcept; // O(log n)
    uint64_t revision() const noexcept { return revision_; }

    // ---- 帧路径（渲染线程，须在 state mutex 内） ----
    // 宽度或主题纪元变化 → 全部块 O(文本) 一次无分配重数；
    // 否则只对置脏块做增量的尾部重扫。
    void begin_frame(int width, uint32_t theme_epoch);
    // 对外行号是「可见行号」：0 = 现存最旧一行。头部裁剪只更新全局
    // 偏移量 base_rows_，前缀和里的绝对行号不平移（§8.3）。
    size_t total_rows() const noexcept {
        return prefix_.back() - base_rows_;
    }
    // 锚点解析：块被头部裁剪后返回 nullopt。
    std::optional<size_t> row_of(uint64_t block_id,
                                 size_t row_in_block) const noexcept;
    // 调用者保证 row < total_rows()。
    Location location_of(size_t row) const noexcept;
    // 物化与 [first, first+count) 相交的块。
    void materialize_range(size_t first, size_t count, const Theme& theme);
    const Line* line_at(size_t row) const noexcept; // 未物化返回 nullptr
    // 释放可见窗口 ± count 行以外块的 rows（open 块除外，见 .cpp）。
    void evict_outside(size_t first, size_t count) noexcept;

    // ---- 渲染器注册（扩展点） ----
    void set_renderer(BlockKind kind, std::unique_ptr<BlockRenderer> renderer);
    const BlockRenderer& renderer(BlockKind kind) const noexcept;

private:
    void count_full(Block& b);
    void count_incremental(Block& b);
    void rebuild_prefix(size_t from);
    void ensure_rows(Block& b, const Theme& theme);
    std::optional<size_t> index_of(uint64_t id) const noexcept;

    std::deque<Block> blocks_;
    std::deque<size_t> prefix_; // 长度 == blocks_+1；值为绝对行号（不因裁剪平移）
    std::array<std::unique_ptr<BlockRenderer>, k_block_kind_count> renderers_{};
    uint64_t next_id_ = 1;
    uint64_t revision_ = 0;   // 任何内容变更后递增（Scrollback 的脏判定）
    size_t first_dirty_ = static_cast<size_t>(-1);
    size_t base_rows_ = 0;    // 已裁剪掉的行数：绝对行号 = 可见行号 + 它
    int width_ = 0;
    uint32_t theme_epoch_ = 0;
};

// 大内容滚动区（§八）：Document 的门面 widget。
// 渲染时按视口宽度折行、只物化可见窗口；内容变更由 revision 驱动重画，
// 不需要调用方手动 invalidate（invalidate 仍然有效，用于主题等纯视图变化）。
class Scrollback : public Widget {
public:
    Document& document() noexcept { return doc_; }
    const Document& document() const noexcept { return doc_; }

    // 主题样式变化后必须递增 Theme::epoch，块的物化缓存随之失效。
    void set_theme(Theme theme) {
        theme_ = std::move(theme);
        invalidate();
    }
    const Theme& theme() const noexcept { return theme_; }

    // ---- 滚动（L6 事件路由调用；正数 = 向下/新内容方向） ----
    void scroll_lines(int lines);
    void scroll_pages(int pages);
    void scroll_home();
    void scroll_end(); // 贴底并保持跟随

    [[nodiscard]] bool pinned() const noexcept {
        return anchor_.pinned_to_bottom;
    }
    // 上次渲染时视口下方的行数（不贴底时 > 0，可用于「有 N 行新内容」提示）。
    [[nodiscard]] size_t unseen_rows() const noexcept { return unseen_; }

    Size measure(Size available) const override;
    void render(Surface& s) override;
    bool dirty_tree() const noexcept override;

private:
    size_t max_top() const noexcept;
    void anchor_to(size_t row);

    Document doc_;
    Theme theme_{};
    Anchor anchor_{};
    uint64_t rendered_rev_ = 0; // 已渲染到的内容版本
    size_t total_ = 0;          // 上次渲染的文档总行数
    size_t top_ = 0;            // 上次渲染的视口顶行
    size_t unseen_ = 0;         // 视口下方行数
    int view_ = 0;              // 上次渲染的视口高度
};

} // namespace dagent::tui
