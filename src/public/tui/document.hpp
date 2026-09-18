// L5 文档层：大内容滚动区（设计文档 §10）。
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
// 语法高亮…）只需实现渲染器并注册，不触碰框架其他部分（§10.7）。
//
// 线程纪律：Document 与控件树只属于渲染线程（02 §4），本层零锁。
// 业务线程经 Runtime::post 提交变更，fn 在渲染线程上执行；这里只做
// 纯内存操作：变更侧置脏，渲染侧推导。
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

// 块的内容形态。默认注册表：text/output → 文本渲染（主题槽位不同），
// code → 语法高亮（meta 是语言），diff → 差异渲染，markdown → 行内样式，
// table → 表格（§10.8）。
enum class BlockKind : uint8_t { text, code, diff, output, markdown, table };
inline constexpr std::size_t k_block_kind_count = 6;

// 物化行的一段连续同样式文本。制表符已展开为空格（§5.2：网格里不存 '\t'，
// 否则每次列计算都要回溯），可直接交给 Surface::text。
inline constexpr size_t k_no_src = static_cast<size_t>(-1);

struct Span {
    std::string text;
    Style style{};
    // 首字节在 source 中的偏移（§10.10 选择）：显示文本与源文本从这里起逐字素
    // 对应（制表符展开为空格除外）。k_no_src = 无源的装饰（列表符号、引用
    // 竖线、表格边框），选择时归属于其后的内容。
    size_t src = k_no_src;
};

// 物化行（屏幕行）。spans 支持一行内多段样式；增量物化会原地复用 Line
// 对象（覆盖写入保留 string 容量），流式帧路径因此不产生新分配。
struct Line {
    std::vector<Span> spans;
    int width = 0;     // 显示宽度（列）
    size_t offset = 0; // 行首在 source 中的字节偏移（锚点解析用）
    // 所在逻辑行末尾的跨行词法状态（低 4 位模式、高 4 位参数），语法渲染器
    // 写入：增量重扫回退到最后一条逻辑行的行首，从上一逻辑行的状态继续，
    // 块注释/三引号/原始字符串跨行不重扫全文（§10.8）。其他渲染器不解释。
    uint8_t lex = 0;
};

// 逻辑块（§10.1）。source 是逻辑原文，未折行、未上色。
struct Block {
    // ---- 逻辑内容 ----
    uint64_t id = 0;          // 单调递增，永不复用；锚点引用它
    BlockKind kind = BlockKind::text;
    std::string source;
    std::string meta;         // 标题、来源等附属信息（默认渲染器不解释）
    uint32_t group = 0;       // 分组（例如并发子任务）
    uint8_t depth = 0;        // 缩进层级（默认渲染器不解释，留给自定义渲染器）
    // 上边距：块前的空行数（段落间距）。由产生块的一方设置 —— 只有它知道
    // 语义（同一消息内的块之间、消息之间）；Document 把它计入行数前缀和
    // 并在块前补空行，渲染器不感知。块没有内容行时边距不生效（空块的
    // 边距折叠，流式新开的空块不会先冒出一行空白）。
    uint8_t margin_top = 0;
    bool open = false;        // 仍在增长（只有尾部块适合，见 append）
    bool collapsed = false;
    uint32_t collapsed_rows = 0; // 折叠后最多显示的行数

    // ---- 派生缓存（Document 维护，调用方只读） ----
    int64_t cache_key = -1;   // 宽度 + 折叠状态 + 主题纪元 的打包键
    size_t row_count = 0;     // 全程行数（未折叠时的计数级结果）
    size_t stable_rows = 0;   // [0, stable_rows) 行的断点已定（§10.3）
    size_t stable_bytes = 0;  // stable_rows 行的起始字节，即计数级折行锚点
    size_t rows_valid = 0;    // rows 中与当前 source 一致的前缀行数
    size_t rows_bytes = 0;    // rows_valid 前缀覆盖到的 source 字节数（物化锚点）
    std::vector<Line> rows;   // 物化结果；被驱逐/失效时 rows_valid 归零
};

// 语义主题令牌（§9.2）定义在 L4 的 widget.hpp：控件与渲染器共用同一套
// 令牌，任何样式字段变更后必须递增 epoch，否则块的物化缓存（cache_key
// 含 epoch）不会失效。

// 滚动锚点（§10.5）。贴底是独立状态而不是「偏移量为 0」：新内容到达时
// 贴底则跟随，不贴底则保持锚点不动。位置用块内字节偏移而不是块内行号：
// 行号随宽度重折而变，字节偏移不变，改变宽度后视口仍停在同一段内容上。
struct Anchor {
    uint64_t block_id = 0;
    size_t byte_in_block = 0;
    bool pinned_to_bottom = true;
};

// 行的内容位置：所在块 + 行首字节偏移（行号由 Document::row_of 换算）。
// 块 id 单调递增、块按 id 有序，因此按 (块 id, 字节) 比较即文档顺序。
struct Location {
    uint64_t block_id = 0;
    size_t byte_in_block = 0;
    auto operator<=>(const Location&) const = default;
};

// 选区（§10.10）：两个逻辑位置，都指向字素起点，闭区间（含两端的字素）。
// 用块 id + 字节偏移表示，改变宽度后仍覆盖同一段内容。
struct Selection {
    Location anchor;
    Location head;
};

// 折行计数结果（§10.2/§10.3）。除最后一行外，每一行的断点都由已有字符
// 唯一决定：stable_rows/stable_bytes 就是「已定」前缀的边界，
// 追加内容只需从 stable_bytes 重扫最后一行。
struct WrapResult {
    size_t rows = 0;         // 显示行数（含结尾不完整行）
    size_t stable_rows = 0;  // 断点已定的行数
    size_t stable_bytes = 0; // 相对计数起点的偏移：stable_rows 的下一行起点
};

// 从 source[from] 起只数行、零分配（§10.2 的计数级接口）；from 必须是行边界，
// 返回值里的 stable_bytes 相对 from。
WrapResult wrap_measure_from(std::string_view source, size_t from,
                             int width) noexcept;
size_t count_rows(std::string_view source, int width) noexcept;

// 单行折行结果。与 wrap_measure_from 使用同一套断行决策，供自定义
// BlockRenderer 的物化循环复用（§10.7）：按行推进的渲染器必须与计数走
// 同一套边界，否则 measure 与 render 的行数不再相等。
struct RowEdge {
    size_t end = 0;   // 本行内容为 [from, end)
    size_t next = 0;  // 下一行起始字节（换行结束则跨过换行符）
    int width = 0;    // 本行显示宽度
    size_t cut = 0;   // 超宽断行时触发断行的字素起点（无断行时无意义）
};
RowEdge wrap_next_row(std::string_view source, size_t from, int width) noexcept;

// 把 source[begin, end) 展开为可绘制文本写入 dst（清空、保留容量）：
// 制表符按 wrap_next_row 的同一套列推进展开为空格，其余字节原样。
void expand_row(std::string& dst, std::string_view source, size_t begin,
                size_t end);

// 块渲染器：把 source 变成带样式的行（§10.7）。实现必须满足
// measure(s, 0, w).rows == render 产出的行数（§10.2 的等价性）。
class BlockRenderer {
public:
    virtual ~BlockRenderer() = default;

    // 只数行、不分配任何字符串；Document 的全量与增量计数都经由它。
    // [0, stable_rows) 行在追加内容后必须保持不变；做不到的实现返回
    // stable_rows = stable_bytes = 0，退化为每次从头计数与物化。
    // 默认实现即标准折行（wrap_measure_from）。
    virtual WrapResult measure(std::string_view source, size_t from, int width) const {
        return wrap_measure_from(source, from, width);
    }

    // 从 source[from] 起折行/上色，覆盖写入 out[valid...]；out 的
    // [0, valid) 前缀由调用方保证仍然有效。返回写入后的有效行数。
    // 折叠块只产出 min(collapsed_rows, row_count) 行。
    virtual size_t render(const Block& block, int width, const ThemeTokens& theme,
                          size_t from, size_t valid,
                          std::vector<Line>& out) const = 0;
};

// 文本渲染器：每行一段，样式取自 ThemeTokens 的指定槽位（成员指针）。
class TextRenderer final : public BlockRenderer {
public:
    explicit TextRenderer(Style ThemeTokens::*slot = &ThemeTokens::text) noexcept
        : slot_(slot) {}

    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;

private:
    Style ThemeTokens::*slot_;
};

// 差异渲染器：'+' / '-' / '@' 开头的行分别用 diff_added / diff_removed /
// diff_hunk 令牌，其余正文。
class DiffRenderer final : public BlockRenderer {
public:
    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;
};

// Markdown 渲染器（§10.8）：块级前缀（标题 #、列表 •、引用 │、分隔线）
// 用 dim 显示，行内标记（粗体/斜体/行内代码/链接/转义）隐藏，只显示带样式
// 的正文（链接地址不显示）。按段（起始行 + 续行）解析，强调可跨软换行，
// 折行按隐藏标记后的显示文本进行，计数与物化共用同一排版函数。整块每次
// 重排（stable_rows = 0，代价受段落长度约束）。
class MarkdownRenderer final : public BlockRenderer {
public:
    WrapResult measure(std::string_view source, size_t from,
                       int width) const override;
    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;
};

// 表格渲染器（§10.8）：列宽依赖全部行，每次变化整块重排。列宽超出可用
// 宽度时按比例收缩，单元格超宽截断并以 '…' 结束。行数 = 逻辑行数（不折行）。
class TableRenderer final : public BlockRenderer {
public:
    WrapResult measure(std::string_view source, size_t from,
                       int width) const override;
    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;
};

// 轻量语法高亮渲染器（§10.9）：语言取自 Block::meta 的首个词，覆盖
// C/C++、Python、JavaScript/TypeScript、JSON、Bash、Go、Rust，未识别按
// 主题 code 槽纯文本。按整条逻辑行做词法分析、再按折行切片（行注释与
// 字符串不会被折行打断），跨行词法状态写进 Line::lex，已定行保持增量：
// 流式追加后只重扫最后一条逻辑行（§10.8）。不引入 tree-sitter。
// 视觉区分：每行左侧一道竖条（border_active）+ 整行铺 background_element
// 底色；竖条占 2 列，计数与物化都按「宽度 − 2」折行。
class SyntaxRenderer final : public BlockRenderer {
public:
    static constexpr int k_gutter = 2;

    WrapResult measure(std::string_view source, size_t from, int width) const override;
    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
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

    // ---- 内容变更（渲染线程上执行，业务线程经 Runtime::post 提交）----
    // 只改内存并置脏，绝不 I/O。
    uint64_t append_block(BlockKind kind, std::string source = {});
    uint64_t append_block(Block block); // 完整控制 meta/group/depth/open/collapsed
    // 建一个可增长的块（流式输出的入口，配合 append/close_block）。
    // 多个块可以同时 open，尾部之外的中部块也一样（§10.6）。
    uint64_t open_block(BlockKind kind, std::string source = {});
    // 追加到 open 块；只 source += chunk（O(chunk)，不折行、零分配）。
    // id 不存在或块已关闭时返回 false。
    bool append(uint64_t id, std::string_view chunk);
    // 任意块整体替换 source（工具状态回写、重试输出）。重置该块缓存，
    // 折行在渲染时进行；锚点在该块时字节偏移夹到新长度（§10.6）。
    bool replace(uint64_t id, std::string source);
    // 只改 meta 并失效该块物化缓存（计数不受影响）。
    bool set_meta(uint64_t id, std::string meta);
    // 删除 id 及其后所有块（/undo），返回删除数。锚点块被删除时移到
    // 删除点之前最后一块的末尾；文档已空则贴底（§10.6）。
    size_t erase_from(uint64_t id);
    bool close_block(uint64_t id); // 终结增长：行数从此不再变
    bool set_collapsed(uint64_t id, bool collapsed, uint32_t rows);

    void clear();
    void trim_blocks(size_t keep); // 头部裁剪到至多 keep 块，O(1)/块
    void trim_rows(size_t keep);   // 头部裁剪到至多 keep 行，O(1)/块

    // 滚动锚点（§10.5）由文档持有：replace/erase_from 自动维护其有效性，
    // Scrollback 只读取与重定位。
    const Anchor& anchor() const noexcept { return anchor_; }
    void set_anchor(Anchor anchor) noexcept { anchor_ = anchor; }

    size_t block_count() const noexcept { return blocks_.size(); }
    const Block& block_at(size_t index) const noexcept { return blocks_[index]; }
    const Block* find(uint64_t id) const noexcept; // O(log n)
    uint64_t revision() const noexcept { return revision_; }

    // ---- 帧路径（渲染线程）----
    // 宽度或主题纪元变化 → 全部块 O(文本) 一次无分配重数；
    // 否则只对置脏块做增量的尾部重扫。
    void begin_frame(int width, uint32_t theme_epoch);
    // 对外行号是「可见行号」：0 = 现存最旧一行。头部裁剪只更新全局
    // 偏移量 base_rows_，前缀和里的绝对行号不平移（§10.4）。
    size_t total_rows() const noexcept {
        return prefix_.back() - base_rows_;
    }
    // 锚点解析：包含该字节的行（块被头部裁剪后返回 nullopt）。
    // 两者都会物化所在块 —— 锚点所在块就是可见块，本来就要物化。
    std::optional<size_t> row_of(uint64_t block_id, size_t byte_in_block,
                                 const ThemeTokens& theme);
    // 调用者保证 row < total_rows()。边距行没有内容，返回其所在块的块首
    // （即边距下方的第一行内容）。
    Location location_of(size_t row, const ThemeTokens& theme);
    // 物化与 [first, first+count) 相交的块。
    void materialize_range(size_t first, size_t count, const ThemeTokens& theme);
    // 未物化返回 nullptr；边距行返回一个空行。
    const Line* line_at(size_t row) const noexcept;
    // 释放可见窗口 ± count 行以外块的 rows（open 块除外，见 .cpp）。
    void evict_outside(size_t first, size_t count) noexcept;

    // ---- 选择（§10.10） ----
    // 屏幕位置 → 逻辑位置：第 row 行第 col 列处字素的源字节。装饰（列表
    // 符号等）归属于其后的内容；行尾之后在硬换行/块尾处取行尾（含换行），
    // 在软折行处取行内最后一个字素。边距行取块首。调用者保证 row < total_rows()。
    Location location_at(size_t row, int col, const ThemeTokens& theme);
    // [a, b] 之间的源文本（Markdown 原文，含 b 处的字素；a、b 次序任意）。
    // 软折行不插入换行；跨块时补齐块间换行，有上边距的块前空一行。
    // 任一端的块已不存在时返回空串。
    std::string text_between(Location a, Location b) const;
    // 双击选词：loc 所在的词（字母数字、'_' 与非 ASCII 字符的连续段）；
    // 不在词上时只选该字素。三击选逻辑行（不含行尾换行）。
    Selection word_around(Location loc) const;
    Selection line_around(Location loc) const;

    // ---- 渲染器注册（扩展点） ----
    void set_renderer(BlockKind kind, std::unique_ptr<BlockRenderer> renderer);
    const BlockRenderer& renderer(BlockKind kind) const noexcept;

private:
    void count_full(Block& b);
    void count_incremental(Block& b);
    void rebuild_prefix(size_t from);
    void ensure_rows(Block& b, const ThemeTokens& theme);
    std::optional<size_t> index_of(uint64_t id) const noexcept;

    std::deque<Block> blocks_;
    std::deque<size_t> prefix_; // 长度 == blocks_+1；值为绝对行号（不因裁剪平移）
    std::array<std::unique_ptr<BlockRenderer>, k_block_kind_count> renderers_{};
    Anchor anchor_{};         // 滚动锚点（replace/erase_from 自动维护）
    uint64_t next_id_ = 1;
    uint64_t revision_ = 0;   // 任何内容变更后递增（Scrollback 的脏判定）
    size_t first_dirty_ = static_cast<size_t>(-1);
    size_t base_rows_ = 0;    // 已裁剪掉的行数：绝对行号 = 可见行号 + 它
    int width_ = 0;
    uint32_t theme_epoch_ = 0;
};

// 流式 Markdown 切分器（§10.8，L5，渲染线程使用）。把一条持续增长的
// Markdown 消息切成多个 Document 块，只有最后一个块在增长：
//   * 段落/标题/列表/引用 → markdown（空行或下一行开启其他块级结构时关闭）；
//   * 围栏代码（``` / ~~~）→ code，info 串存入 meta（围栏行不属于 source）；
//   * 表格（表头 + 分隔行）→ table（空行或不再以 '|' 开头的行结束）；
//   * 分隔线 → markdown，立即关闭。
// 消息内的块之间空 1 行（Block::margin_top），源码里的空行不进块。
//
// 未完成的行先追加到当前块立即显示；整行到达后若判定它开启了新结构，则用
// replace 把它从当前块移除（整块替换，O(当前块)）。列表项与引用的续行
// 留在当前块。切分结果与喂入分块
// 无关：所有结构判定都发生在完整行上。切分器只改自己记下的当前块，
// 多条流或普通追加交错写入互不影响。
//
// finish() 之后状态复位，同一实例可以开始下一条消息。
class MarkdownStream {
public:
    // first_margin：每条消息第一块的上边距（消息之间的间距，由应用决定）；
    // 消息内其后的块上边距为 1（段落间距，Block::margin_top）。
    explicit MarkdownStream(Document& doc, uint8_t first_margin = 0) noexcept
        : doc_(&doc), first_margin_(first_margin) {}
    MarkdownStream(const MarkdownStream&) = delete;
    MarkdownStream& operator=(const MarkdownStream&) = delete;

    // 追加任意分块的原文。
    void feed(std::string_view chunk);
    // 消息结束：未完成的行按现状保留在块内，关闭最后一个块。
    void finish();

private:
    enum class Mode : uint8_t { none, markdown, code, table };
    enum class Family : uint8_t { text, heading, list, quote };

    void on_partial();                     // pending_ 增长（尚无换行）
    void on_line(bool has_newline);        // pending_ 是一整行
    void handle_markdown_line(std::string_view line, bool has_newline);
    void handle_detached_line(std::string_view line, bool has_newline,
                              bool allow_table);
    void open_markdown(std::string source, bool pipe);
    void open_table(std::string source);
    void start_code(char fence, size_t len, std::string info);
    Block new_block(BlockKind kind); // 按消息内位置设置上边距
    void close_current();
    void remove_last(size_t bytes);

    Document* doc_;
    Mode mode_ = Mode::none;
    Family family_ = Family::text;
    uint64_t cur_ = 0;             // 当前 open 块
    char fence_ch_ = 0;            // 代码围栏字符（` 或 ~）
    size_t fence_len_ = 0;         // 围栏长度（闭合需 >=）
    bool code_candidate_ = false;  // 代码块中当前未完成行可能是闭合围栏
    bool line_in_block_ = false;   // 当前行已（部分）写入当前块
    bool line_new_ = false;        // 当前行的块是行中途新建的（分类时不回退）
    bool last_line_pipe_ = false;  // 当前 markdown 块最后一行以 '|' 开头
    size_t last_line_len_ = 0;     // 该行字节数（含换行）
    size_t sent_ = 0;              // pending_ 中已追加到当前块的字节数
    std::string pending_;          // 未完成行（原始字节，不含换行）
    std::string staged_;           // 模式 none 下暂存的表格候选表头（含换行）
    bool staging_ = false;
    bool started_ = false;         // 本条消息已建过块（其后的块带段落间距）
    uint8_t first_margin_ = 0;
};

// 大内容滚动区（§10）：Document 的门面 widget。
// 渲染时按视口宽度折行、只物化可见窗口；内容变更由 revision 驱动重画，
// 不需要调用方手动 invalidate（invalidate 仍然有效，用于主题等纯视图变化）。
class Scrollback : public Widget {
public:
    Document& document() noexcept { return doc_; }
    const Document& document() const noexcept { return doc_; }

    // 主题样式变化后必须递增 ThemeTokens::epoch，块的物化缓存随之失效。
    void set_theme(ThemeTokens theme) {
        theme_ = std::move(theme);
        invalidate();
    }
    const ThemeTokens& theme() const noexcept { return theme_; }

    // ---- 滚动（L6 事件路由调用；正数 = 向下/新内容方向） ----
    void scroll_lines(int lines);
    void scroll_pages(int pages);
    void scroll_home();
    void scroll_end(); // 贴底并保持跟随

    [[nodiscard]] bool pinned() const noexcept {
        return doc_.anchor().pinned_to_bottom;
    }
    // 上次渲染时视口下方的行数（不贴底时 > 0，可用于「有 N 行新内容」提示）。
    [[nodiscard]] size_t unseen_rows() const noexcept { return unseen_; }

    // ---- 选择（§10.10）：选区只影响本视图的绘制（叠加反色），不写进
    // Document 的物化缓存；变化时只失效本 widget。 ----
    // 视图内坐标 → 逻辑位置（按上次渲染的视口；越界夹到视口内，拖出
    // 视口时选区延伸到可见的首/末行）。文档为空时返回 nullopt。
    std::optional<Location> hit(Point p);
    void select(Selection s);
    void clear_selection();
    [[nodiscard]] const std::optional<Selection>& selection() const noexcept {
        return selection_;
    }
    // 选区的源文本（Document::text_between）；无选区时为空串。
    [[nodiscard]] std::string selected_text() const;

    Size measure(Size available) const override;
    void render(Surface& s) override;
    bool dirty_tree() const noexcept override;

private:
    void draw_row(Surface& s, int y, size_t row, const Line& ln);
    size_t max_top() const noexcept;
    // dir < 0：向上滚动（落在边距行时越过边距，锚到上一块的末行）。
    void anchor_to(size_t row, int dir);

    Document doc_;
    ThemeTokens theme_ = dark_theme();
    uint64_t rendered_rev_ = 0; // 已渲染到的内容版本
    size_t total_ = 0;          // 上次渲染的文档总行数
    size_t top_ = 0;            // 上次渲染的视口顶行
    size_t unseen_ = 0;         // 视口下方行数
    int view_ = 0;              // 上次渲染的视口高度
    std::optional<Selection> selection_;
};

} // namespace dagent::tui
