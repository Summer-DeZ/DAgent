// L5 文档实现：前缀和定位、锚点、增量物化与驱逐、Scrollback widget。
// 全部操作都是纯内存操作；跨线程顺序由 L7 的 state mutex 保证（§十），
// 本文件不加锁、不做 I/O。
#include "tui/document.hpp"

#include <algorithm>
#include <utility>

namespace dagent::tui {

namespace {

constexpr size_t k_npos = static_cast<size_t>(-1);

// cache_key 打包：宽度 16 位 | 折叠 1 位 | 折叠行数 15 位 | 主题纪元 32 位。
// 实际取值域远小于各字段位宽，键相等即缓存可用。
int64_t block_key(const Block& b, int width, uint32_t epoch) noexcept {
    const uint64_t w = static_cast<uint32_t>(width) & 0xFFFFu;
    const uint64_t c = b.collapsed ? 1u : 0u;
    const uint64_t cr = static_cast<uint64_t>(b.collapsed_rows) & 0x7FFFu;
    return static_cast<int64_t>(w | (c << 16) | (cr << 17) |
                                (static_cast<uint64_t>(epoch) << 32));
}

// 显示行数：折叠时是截断后的计数，未折叠时等于全程计数。
size_t display_rows(const Block& b) noexcept {
    return b.collapsed ? std::min<size_t>(b.collapsed_rows, b.row_count)
                       : b.row_count;
}

} // namespace

Document::Document() {
    renderers_[static_cast<size_t>(BlockKind::text)] =
        std::make_unique<TextRenderer>(&Theme::text);
    renderers_[static_cast<size_t>(BlockKind::code)] =
        std::make_unique<TextRenderer>(&Theme::code);
    renderers_[static_cast<size_t>(BlockKind::diff)] =
        std::make_unique<DiffRenderer>();
    renderers_[static_cast<size_t>(BlockKind::output)] =
        std::make_unique<TextRenderer>(&Theme::dim);
    prefix_.push_back(0);
}

// ---- 内容变更（业务线程） ----

uint64_t Document::append_block(BlockKind kind, std::string source) {
    Block b;
    b.kind = kind;
    b.source = std::move(source);
    return append_block(std::move(b));
}

uint64_t Document::open_block(BlockKind kind, std::string source) {
    Block b;
    b.kind = kind;
    b.source = std::move(source);
    b.open = true;
    return append_block(std::move(b));
}

// 新区块立即计数（O(该块文本)），但不到可见之前不物化（§8.2）。
uint64_t Document::append_block(Block block) {
    block.id = next_id_++;
    block.cache_key = -1;
    block.rows_valid = 0;
    block.rows_bytes = 0;
    block.rows.clear();
    blocks_.push_back(std::move(block));
    Block& b = blocks_.back();
    count_full(b);
    prefix_.push_back(prefix_.back() + display_rows(b));
    ++revision_;
    return b.id;
}

// §8.5：追加只改 source 并置脏，不折行、不分配、零 I/O。
bool Document::append(uint64_t id, std::string_view chunk) {
    const auto idx = index_of(id);
    if (!idx || !blocks_[*idx].open) return false;
    if (chunk.empty()) return true;
    blocks_[*idx].source.append(chunk);
    if (*idx < first_dirty_) first_dirty_ = *idx;
    ++revision_;
    return true;
}

bool Document::close_block(uint64_t id) {
    const auto idx = index_of(id);
    if (!idx) return false;
    Block& b = blocks_[*idx];
    if (!b.open) return true;
    b.open = false;
    if (*idx < first_dirty_) first_dirty_ = *idx;
    ++revision_;
    return true;
}

bool Document::set_collapsed(uint64_t id, bool collapsed, uint32_t rows) {
    const auto idx = index_of(id);
    if (!idx) return false;
    Block& b = blocks_[*idx];
    if (b.collapsed == collapsed && b.collapsed_rows == rows) return true;
    b.collapsed = collapsed;
    b.collapsed_rows = rows;
    // cache_key 含折叠状态：置脏后由 begin_frame 走整块重数路径。
    if (*idx < first_dirty_) first_dirty_ = *idx;
    ++revision_;
    return true;
}

void Document::clear() {
    blocks_.clear();
    prefix_.clear();
    prefix_.push_back(0);
    base_rows_ = 0;
    first_dirty_ = k_npos;
    ++revision_; // id 序列保持单调，不复用
}

// 头部裁剪 O(1)/块：只弹出并抬高全局行偏移，不平移前缀和（§8.3）。
void Document::trim_blocks(size_t keep) {
    if (blocks_.size() <= keep) return;
    while (blocks_.size() > keep) {
        blocks_.pop_front();
        prefix_.pop_front();
        if (first_dirty_ != k_npos && first_dirty_ > 0) --first_dirty_;
        ++revision_;
    }
    base_rows_ = prefix_.front(); // 绝对行号 = 可见行号 + base_rows_
}

void Document::trim_rows(size_t keep) {
    // 丢弃首块后剩余行数 = prefix_.back() - prefix_[1]；保持剩余 >= keep。
    while (blocks_.size() > 1 && prefix_.back() - prefix_[1] >= keep) {
        blocks_.pop_front();
        prefix_.pop_front();
        if (first_dirty_ != k_npos && first_dirty_ > 0) --first_dirty_;
        ++revision_;
    }
    base_rows_ = prefix_.front();
}

const Block* Document::find(uint64_t id) const noexcept {
    const auto idx = index_of(id);
    return idx ? &blocks_[*idx] : nullptr;
}

// id 单调递增 → blocks_ 按 id 有序，二分定位（§8.3）。
std::optional<size_t> Document::index_of(uint64_t id) const noexcept {
    const auto it = std::lower_bound(
        blocks_.begin(), blocks_.end(), id,
        [](const Block& b, uint64_t v) { return b.id < v; });
    if (it == blocks_.end() || it->id != id) return std::nullopt;
    return static_cast<size_t>(it - blocks_.begin());
}

// ---- 帧路径（渲染线程） ----

void Document::begin_frame(int width, uint32_t theme_epoch) {
    if (width != width_ || theme_epoch != theme_epoch_) {
        // 宽度/主题纪元变化：折行与物化缓存整体作废，O(全部块文本)
        // 一次无分配重数；物化仍只发生在可见窗口（§十一）。
        width_ = width;
        theme_epoch_ = theme_epoch;
        for (Block& b : blocks_) count_full(b);
        rebuild_prefix(0);
        first_dirty_ = k_npos;
        return;
    }
    if (first_dirty_ == k_npos) return;
    for (size_t i = first_dirty_; i < blocks_.size(); ++i) {
        Block& b = blocks_[i];
        // 折叠视图是截断，增量锚点无意义；cache_key 变化（折叠开关）
        // 同样走整块重数。
        if (b.collapsed || b.cache_key != block_key(b, width, theme_epoch)) {
            count_full(b);
        } else {
            count_incremental(b);
        }
    }
    rebuild_prefix(first_dirty_);
    first_dirty_ = k_npos;
}

// 整块重数：折行全量扫一遍（零分配），物化缓存全部作废。
void Document::count_full(Block& b) {
    const WrapResult r = wrap_measure(b.source, width_);
    b.row_count = r.rows;
    b.stable_rows = r.stable_rows;
    b.stable_bytes = r.stable_bytes;
    if (!b.open) {
        // 封闭块不会再有追加：全部行都是稳定的。
        b.stable_rows = b.row_count;
        b.stable_bytes = b.source.size();
    }
    b.cache_key = block_key(b, width_, theme_epoch_);
    b.rows.clear();
    b.rows_valid = 0;
    b.rows_bytes = 0;
}

// 增量重数：只重扫 stable_bytes 之后的最后一个不完整行，O(最后一行)。
void Document::count_incremental(Block& b) {
    // 渲染过但内容已过时的不稳定尾行作废；稳定前缀保留，物化锚点随之回退。
    if (b.rows_valid > b.stable_rows) {
        b.rows_valid = b.stable_rows;
        b.rows_bytes = b.stable_bytes;
    }
    const WrapResult r = wrap_measure_from(b.source, b.stable_bytes, width_);
    b.row_count = b.stable_rows + r.rows;
    b.stable_rows += r.stable_rows;
    b.stable_bytes += r.stable_bytes;
    if (!b.open) {
        b.stable_rows = b.row_count;
        b.stable_bytes = b.source.size();
    }
}

// 只重建受影响的后缀（尾部追加时是 O(1)）。
void Document::rebuild_prefix(size_t from) {
    for (size_t i = from; i < blocks_.size(); ++i) {
        prefix_[i + 1] = prefix_[i] + display_rows(blocks_[i]);
    }
}

std::optional<size_t> Document::row_of(uint64_t block_id,
                                       size_t row_in_block) const noexcept {
    const auto idx = index_of(block_id);
    if (!idx) return std::nullopt; // 块已被头部裁剪
    const Block& b = blocks_[*idx];
    const size_t n = display_rows(b);
    if (n == 0) return prefix_[*idx] - base_rows_;
    return prefix_[*idx] - base_rows_ +
           std::min(row_in_block, n - 1);
}

Location Document::location_of(size_t row) const noexcept {
    const size_t abs = row + base_rows_;
    size_t i = static_cast<size_t>(
        std::upper_bound(prefix_.begin(), prefix_.end(), abs) -
        prefix_.begin());
    --i; // 调用者保证 abs < prefix_.back()，因此 i >= 1
    return {blocks_[i].id, abs - prefix_[i]};
}

void Document::materialize_range(size_t first, size_t count,
                                 const Theme& theme) {
    if (count == 0 || blocks_.empty()) return;
    const size_t first_abs = first + base_rows_;
    const size_t last_abs = first_abs + count;
    size_t i = static_cast<size_t>(
        std::upper_bound(prefix_.begin(), prefix_.end(), first_abs) -
        prefix_.begin());
    if (i > 0) --i;
    for (; i < blocks_.size() && prefix_[i] < last_abs; ++i) {
        ensure_rows(blocks_[i], theme);
    }
}

// 物化一个块到「当前应有的有效行数」。两条路径：
//   * 已完整物化 → 什么都不做；
//   * 否则从 rows_bytes（有效前缀之后）继续画到末尾；
//     被驱逐时 rows_valid/rows_bytes 已归零，等价于从头物化。
void Document::ensure_rows(Block& b, const Theme& theme) {
    const size_t want = display_rows(b);
    if (b.rows_valid == want) return;
    b.rows_valid = renderer(b.kind).render(b, width_, theme, b.rows_bytes,
                                           b.rows_valid, b.rows);
    // 非折叠渲染一定扫到 source 末尾；折叠是截断视图，不参与增量续画。
    b.rows_bytes = b.collapsed ? 0 : b.source.size();
}

const Line* Document::line_at(size_t row) const noexcept {
    if (blocks_.empty()) return nullptr;
    const size_t abs = row + base_rows_;
    if (abs >= prefix_.back()) return nullptr;
    size_t i = static_cast<size_t>(
        std::upper_bound(prefix_.begin(), prefix_.end(), abs) -
        prefix_.begin());
    --i;
    const Block& b = blocks_[i];
    const size_t r = abs - prefix_[i];
    if (r >= b.rows_valid) return nullptr; // 未物化：调用方先 materialize_range
    return &b.rows[r];
}

// 只保留可见窗口 ± 一屏范围内块的物化结果（§8.2 的驱逐）。
// open 块不驱逐：它的 rows 就是增量折行的锚点，丢掉会把
// 「每帧 O(最后一行)」退化成「每帧 O(全文)」。
void Document::evict_outside(size_t first, size_t count) noexcept {
    const size_t first_abs = first + base_rows_;
    const size_t lo = first_abs > count ? first_abs - count : 0;
    const size_t hi = first_abs + count + count;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        Block& b = blocks_[i];
        if (b.open || b.rows.empty()) continue;
        if (prefix_[i] >= hi || prefix_[i + 1] <= lo) {
            b.rows.clear();
            b.rows.shrink_to_fit();
            b.rows_valid = 0;
            b.rows_bytes = 0;
        }
    }
}

// ---- 渲染器注册 ----

void Document::set_renderer(BlockKind kind, std::unique_ptr<BlockRenderer> r) {
    renderers_[static_cast<size_t>(kind)] = std::move(r);
    // 已有块可能用旧渲染器物化过：整体置脏，下次帧路径重新推导。
    for (Block& b : blocks_) {
        if (b.kind == kind) b.cache_key = -1;
    }
    first_dirty_ = 0;
    ++revision_;
}

const BlockRenderer& Document::renderer(BlockKind kind) const noexcept {
    return *renderers_[static_cast<size_t>(kind)];
}

// ---- Scrollback ----

Size Scrollback::measure(Size available) const { return available; }

// 内容版本变化也视为脏：业务线程 append 后不需要手动 invalidate。
bool Scrollback::dirty_tree() const noexcept {
    return Widget::dirty_tree() || rendered_rev_ != doc_.revision();
}

size_t Scrollback::max_top() const noexcept {
    const size_t h = static_cast<size_t>(view_ > 0 ? view_ : 0);
    return total_ > h ? total_ - h : 0;
}

// 把视口顶行锚到绝对行 row：贴底由 pinned 表示，其余记录 (块 id, 块内行号)。
void Scrollback::anchor_to(size_t row) {
    const size_t max = max_top();
    if (row >= max) {
        anchor_.pinned_to_bottom = true;
        return;
    }
    const Location loc = doc_.location_of(row);
    anchor_.block_id = loc.block_id;
    anchor_.row_in_block = static_cast<uint32_t>(loc.row_in_block);
    anchor_.pinned_to_bottom = false;
}

void Scrollback::scroll_lines(int lines) {
    if (lines == 0) return;
    const long long max = static_cast<long long>(max_top());
    const long long target = static_cast<long long>(top_) + lines;
    anchor_to(static_cast<size_t>(target < 0 ? 0 : (target > max ? max : target)));
    invalidate();
}

void Scrollback::scroll_pages(int pages) {
    if (pages == 0) return;
    scroll_lines(pages * (view_ > 0 ? view_ : 1));
}

void Scrollback::scroll_home() {
    anchor_to(0);
    invalidate();
}

void Scrollback::scroll_end() {
    anchor_ = Anchor{}; // pinned_to_bottom = true
    invalidate();
}

void Scrollback::render(Surface& s) {
    const int w = s.cols();
    const int h = s.rows();
    if (w <= 0 || h <= 0) return;
    doc_.begin_frame(w, theme_.epoch);

    const size_t total = doc_.total_rows();
    const size_t view = static_cast<size_t>(h);
    const size_t max = total > view ? total - view : 0;
    size_t top = max;
    if (!anchor_.pinned_to_bottom) {
        // 锚点解析用块 id，不受重折与裁剪影响（§8.4）。
        const auto r = doc_.row_of(anchor_.block_id, anchor_.row_in_block);
        top = std::min(r.value_or(0), max);
    }

    doc_.materialize_range(top, view, theme_);

    s.fill({0, 0, w, h}, U' ', Style{});
    for (int y = 0; y < h; ++y) {
        const Line* ln = doc_.line_at(top + static_cast<size_t>(y));
        if (ln == nullptr) continue;
        int col = 0;
        for (const Span& sp : ln->spans) {
            col = s.text(col, y, sp.text, sp.style);
        }
        if (col < w) s.fill({col, y, w - col, 1}, U' ', Style{});
    }
    doc_.evict_outside(top, view);

    // 夹取发生时（历史被裁剪、内容缩水）锚点跟随实际视口顶行。
    if (!anchor_.pinned_to_bottom && total > 0) {
        const Location loc = doc_.location_of(top);
        anchor_.block_id = loc.block_id;
        anchor_.row_in_block = static_cast<uint32_t>(loc.row_in_block);
    }

    total_ = total;
    top_ = top;
    view_ = h;
    unseen_ = total > top + view ? total - top - view : 0;
    rendered_rev_ = doc_.revision();
}

} // namespace dagent::tui
