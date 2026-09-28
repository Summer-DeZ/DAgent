#include "tui/document.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "tui/grapheme.hpp"

namespace dagent::tui {

namespace {

constexpr size_t k_npos = static_cast<size_t>(-1);

/// @brief cache_key 打包：宽度 16 位 | 折叠 1 位 | 折叠行数 15 位 | 主题纪元 32 位。
int64_t block_key(const Block& b, int width, uint32_t epoch) noexcept {
    const uint64_t w = static_cast<uint32_t>(width) & 0xFFFFu;
    const uint64_t c = b.collapsed ? 1u : 0u;
    const uint64_t cr = static_cast<uint64_t>(b.collapsed_rows) & 0x7FFFu;
    return static_cast<int64_t>(w | (c << 16) | (cr << 17) |
                                (static_cast<uint64_t>(epoch) << 32));
}

/// @brief 内容行数：折叠时是截断后的计数，未折叠时等于全程计数。
size_t content_rows(const Block& b) noexcept {
    return b.collapsed ? std::min<size_t>(b.collapsed_rows, b.row_count)
                       : b.row_count;
}

/// @brief 生效的上边距：没有内容行的块边距折叠为 0。
size_t margin_rows(const Block& b) noexcept {
    return content_rows(b) > 0 ? b.margin_top : 0;
}

/// @brief 块在前缀和中占的行数 = 上边距 + 内容行。
size_t display_rows(const Block& b) noexcept {
    return margin_rows(b) + content_rows(b);
}

const Line k_margin_line{}; ///< 边距行：无 span 的空行

constexpr int k_tab_stop = 8; ///< 与 wrap.cpp 的制表符展开一致

/// @brief 对物化行的每个显示字素回调 f(col, width, byte, content, grapheme, span)；
/// 返回行内内容在源文本中的结束偏移。
template <class F>
size_t walk_line(const Line& ln, std::string_view src, F&& f) {
    int col = 0;
    size_t byte = ln.offset;
    for (size_t k = 0; k < ln.spans.size(); ++k) {
        const Span& sp = ln.spans[k];
        std::string_view d = sp.text;
        unicode::Grapheme g;
        if (sp.src == k_no_src) {
            size_t deco = byte; // 无源装饰锚到其后第一段内容的源偏移
            for (size_t m = k + 1; m < ln.spans.size(); ++m) {
                if (ln.spans[m].src != k_no_src) {
                    deco = ln.spans[m].src;
                    break;
                }
            }
            while (!d.empty() && unicode::next_grapheme(d, g)) {
                f(col, g.width, deco, false, g.bytes, sp);
                col += g.width;
            }
            continue;
        }
        size_t p = sp.src;
        int tab_from = -1;
        while (!d.empty() && unicode::next_grapheme(d, g)) {
            f(col, g.width, p, true, g.bytes, sp);
            col += g.width;
            if (p >= src.size()) continue;
            if (src[p] == '\t') { // 源 '\t' 对应展开出的多个空格
                if (tab_from < 0) tab_from = col - g.width;
                const int stop = (tab_from / k_tab_stop + 1) * k_tab_stop;
                if (col >= stop || d.empty() || d[0] != ' ') {
                    ++p;
                    tab_from = -1;
                }
                continue;
            }
            std::string_view rest = src.substr(p);
            unicode::Grapheme sg;
            p += unicode::next_grapheme(rest, sg) ? sg.bytes.size() : 1;
        }
        byte = p;
    }
    return byte;
}

bool word_byte(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || u == '_' || (u >= '0' && u <= '9') ||
           (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
}

/// @brief p 之前最后一个字素（此处按 UTF-8 码点近似）的起点；p == from 时返回 from。
size_t prev_char(std::string_view s, size_t from, size_t p) noexcept {
    if (p <= from) return from;
    size_t h = p - 1;
    while (h > from && (static_cast<unsigned char>(s[h]) & 0xC0) == 0x80) --h;
    return h;
}

size_t grapheme_len_at(std::string_view s, size_t p) noexcept {
    if (p >= s.size()) return 0;
    std::string_view rest = s.substr(p);
    unicode::Grapheme g;
    return unicode::next_grapheme(rest, g) ? g.bytes.size() : 1;
}

} // namespace

Document::Document() {
    renderers_[static_cast<size_t>(BlockKind::text)] =
        std::make_unique<TextRenderer>(&ThemeTokens::text);
    renderers_[static_cast<size_t>(BlockKind::code)] =
        std::make_unique<SyntaxRenderer>();
    renderers_[static_cast<size_t>(BlockKind::diff)] =
        std::make_unique<DiffRenderer>();
    renderers_[static_cast<size_t>(BlockKind::output)] =
        std::make_unique<TextRenderer>(&ThemeTokens::text_muted);
    renderers_[static_cast<size_t>(BlockKind::markdown)] =
        std::make_unique<MarkdownRenderer>();
    renderers_[static_cast<size_t>(BlockKind::table)] =
        std::make_unique<TableRenderer>();
    prefix_.push_back(0);
}

// ---- 内容变更（业务线程） ----

uint64_t Document::append_block(BlockKind kind, std::string source) {
    Block b;
    b.kind = kind;
    b.source = std::move(source);
    return append_block(std::move(b));
}

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

bool Document::append(uint64_t id, std::string_view chunk) {
    const auto idx = index_of(id);
    if (!idx || !blocks_[*idx].open) return false;
    if (chunk.empty()) return true;
    blocks_[*idx].source.append(chunk);
    if (*idx < first_dirty_) first_dirty_ = *idx;
    ++revision_;
    return true;
}

bool Document::replace(uint64_t id, std::string source) {
    const auto idx = index_of(id);
    if (!idx) return false;
    Block& b = blocks_[*idx];
    b.source = std::move(source);
    b.cache_key = -1;
    b.stable_rows = 0;
    b.stable_bytes = 0;
    b.rows.clear();
    b.rows_valid = 0;
    b.rows_bytes = 0;
    if (*idx < first_dirty_) first_dirty_ = *idx;
    // 锚点在该块：字节偏移夹到新长度。
    if (!anchor_.pinned_to_bottom && anchor_.block_id == id) {
        anchor_.byte_in_block = std::min(anchor_.byte_in_block, b.source.size());
    }
    ++revision_;
    return true;
}

bool Document::set_meta(uint64_t id, std::string meta) {
    const auto idx = index_of(id);
    if (!idx) return false;
    Block& b = blocks_[*idx];
    b.meta = std::move(meta);
    b.rows.clear();
    b.rows_valid = 0;
    b.rows_bytes = 0;
    ++revision_;
    return true;
}

size_t Document::erase_from(uint64_t id) {
    const auto idx = index_of(id);
    if (!idx) return 0;
    const size_t removed = blocks_.size() - *idx;
    blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(*idx),
                  blocks_.end());
    prefix_.erase(prefix_.begin() + static_cast<std::ptrdiff_t>(*idx) + 1,
                  prefix_.end());
    if (first_dirty_ != k_npos && first_dirty_ >= *idx) {
        first_dirty_ = *idx;
    }
    if (!anchor_.pinned_to_bottom && anchor_.block_id >= id) {
        // 锚点块被删：移到前一块末尾
        if (*idx == 0) {
            anchor_ = Anchor{}; // 文档已空：贴底
        } else {
            const Block& prev = blocks_[*idx - 1];
            anchor_ = Anchor{prev.id, prev.source.size(), false};
        }
    }
    ++revision_;
    return removed;
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
    // cache_key 含折叠状态：置脏走整块重数。
    if (*idx < first_dirty_) first_dirty_ = *idx;
    ++revision_;
    return true;
}

void Document::clear() {
    blocks_.clear();
    prefix_.clear();
    prefix_.push_back(0);
    first_dirty_ = k_npos;
    anchor_ = Anchor{}; // 内容清空：贴底
    ++revision_; // id 序列保持单调，不复用
}

const Block* Document::find(uint64_t id) const noexcept {
    const auto idx = index_of(id);
    return idx ? &blocks_[*idx] : nullptr;
}

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
        // 宽度/主题纪元变化：缓存整体作废并全量重数。
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
        // 折叠或 cache_key 变化（折叠开关）走整块重数，否则增量。
        if (b.collapsed || b.cache_key != block_key(b, width, theme_epoch)) {
            count_full(b);
        } else {
            count_incremental(b);
        }
    }
    rebuild_prefix(first_dirty_);
    first_dirty_ = k_npos;
}

/// @brief 整块重数，并作废该块物化缓存。
void Document::count_full(Block& b) {
    const WrapResult r = renderer(b.kind).measure(b.source, 0, width_);
    b.row_count = r.rows;
    b.stable_rows = r.stable_rows;
    b.stable_bytes = r.stable_bytes;
    if (!b.open) {
        // 封闭块：全部行都稳定。
        b.stable_rows = b.row_count;
        b.stable_bytes = b.source.size();
    }
    b.cache_key = block_key(b, width_, theme_epoch_);
    b.rows.clear();
    b.rows_valid = 0;
    b.rows_bytes = 0;
}

/// @brief 增量重数：只重扫 stable_bytes 之后的不完整行。
void Document::count_incremental(Block& b) {
    // 渲染过但已过时的尾行作废，物化锚点回退到稳定前缀。
    if (b.rows_valid > b.stable_rows) {
        b.rows_valid = b.stable_rows;
        b.rows_bytes = b.stable_bytes;
    }
    const WrapResult r = renderer(b.kind).measure(b.source, b.stable_bytes, width_);
    b.row_count = b.stable_rows + r.rows;
    b.stable_rows += r.stable_rows;
    b.stable_bytes += r.stable_bytes;
    if (!b.open) {
        b.stable_rows = b.row_count;
        b.stable_bytes = b.source.size();
    }
}

/// @brief 从 from 起重建前缀和后缀。
void Document::rebuild_prefix(size_t from) {
    for (size_t i = from; i < blocks_.size(); ++i) {
        prefix_[i + 1] = prefix_[i] + display_rows(blocks_[i]);
    }
}

/// @brief 二分找 byte 所在显示行（行首偏移随行号单调递增）。
std::optional<size_t> Document::row_of(uint64_t block_id, size_t byte_in_block,
                                       const ThemeTokens& theme) {
    const auto idx = index_of(block_id);
    if (!idx) return std::nullopt; // 块已被头部裁剪
    Block& b = blocks_[*idx];
    ensure_rows(b, theme);
    const size_t base = prefix_[*idx] + margin_rows(b);
    const auto first = b.rows.begin();
    const auto last = first + static_cast<std::ptrdiff_t>(b.rows_valid);
    const auto it = std::upper_bound(
        first, last, byte_in_block,
        [](size_t v, const Line& ln) { return v < ln.offset; });
    return base + (it == first ? 0 : static_cast<size_t>(it - first) - 1);
}

Location Document::location_of(size_t row, const ThemeTokens& theme) {
    const size_t abs = row;
    size_t i = static_cast<size_t>(
        std::upper_bound(prefix_.begin(), prefix_.end(), abs) -
        prefix_.begin());
    --i; // 调用者保证 abs < prefix_.back()，因此 i >= 1
    Block& b = blocks_[i];
    const size_t local = abs - prefix_[i];
    const size_t margin = margin_rows(b);
    if (local < margin) return {b.id, 0}; // 边距行：锚到块首
    ensure_rows(b, theme);
    return {b.id, b.rows[local - margin].offset};
}

void Document::materialize_range(size_t first, size_t count,
                                 const ThemeTokens& theme) {
    if (count == 0 || blocks_.empty()) return;
    const size_t first_abs = first;
    const size_t last_abs = first_abs + count;
    size_t i = static_cast<size_t>(
        std::upper_bound(prefix_.begin(), prefix_.end(), first_abs) -
        prefix_.begin());
    if (i > 0) --i;
    for (; i < blocks_.size() && prefix_[i] < last_abs; ++i) {
        ensure_rows(blocks_[i], theme);
    }
}

/// @brief 物化块到当前应有行数：已完整则跳过，否则从 rows_bytes 续画。
void Document::ensure_rows(Block& b, const ThemeTokens& theme) {
    const size_t want = content_rows(b);
    if (b.rows_valid == want) return;
    b.rows_valid = renderer(b.kind).render(b, width_, theme, b.rows_bytes,
                                           b.rows_valid, b.rows);
    // 折叠是截断视图，不参与增量续画。
    b.rows_bytes = b.collapsed ? 0 : b.source.size();
}

const Line* Document::line_at(size_t row) const noexcept {
    if (blocks_.empty()) return nullptr;
    const size_t abs = row;
    if (abs >= prefix_.back()) return nullptr;
    size_t i = static_cast<size_t>(
        std::upper_bound(prefix_.begin(), prefix_.end(), abs) -
        prefix_.begin());
    --i;
    const Block& b = blocks_[i];
    const size_t local = abs - prefix_[i];
    const size_t margin = margin_rows(b);
    if (local < margin) return &k_margin_line;
    const size_t r = local - margin;
    if (r >= b.rows_valid) return nullptr; // 未物化：调用方先 materialize_range
    return &b.rows[r];
}

void Document::evict_outside(size_t first, size_t count) noexcept {
    const size_t first_abs = first;
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

// ---- 选择 ----

Location Document::location_at(size_t row, int col, const ThemeTokens& theme) {
    const size_t abs = row;
    size_t i = static_cast<size_t>(
        std::upper_bound(prefix_.begin(), prefix_.end(), abs) -
        prefix_.begin());
    --i;
    Block& b = blocks_[i];
    const size_t local = abs - prefix_[i];
    const size_t margin = margin_rows(b);
    if (local < margin) return {b.id, 0};
    ensure_rows(b, theme);
    const Line& ln = b.rows[local - margin];
    std::optional<size_t> hit;
    size_t last = ln.offset; // 行内最后一个内容字素
    const size_t end = walk_line(
        ln, b.source,
        [&](int c, int w, size_t byte, bool content, std::string_view, const Span&) {
            if (!hit && w > 0 && col < c + w) hit = byte;
            if (content) last = byte;
        });
    if (hit) return {b.id, *hit};
    const std::string& src = b.source;
    if (end >= src.size() || src[end] == '\n' || src[end] == '\r') return {b.id, end};
    return {b.id, last};
}

std::string Document::text_between(Location a, Location b) const {
    if (b < a) std::swap(a, b);
    const auto ia = index_of(a.block_id);
    const auto ib = index_of(b.block_id);
    if (!ia || !ib) return {};
    std::string out;
    for (size_t i = *ia; i <= *ib; ++i) {
        const Block& blk = blocks_[i];
        const std::string_view src = blk.source;
        const size_t from = i == *ia ? std::min(a.byte_in_block, src.size()) : 0;
        size_t to = src.size();
        if (i == *ib) {
            to = std::min(b.byte_in_block, src.size());
            to += grapheme_len_at(src, to); // 闭区间：含 b 处的字素
        }
        if (i > *ia) {
            if (!out.empty() && out.back() != '\n') out += '\n';
            if (margin_rows(blk) > 0) out += '\n'; // 段落间距
        }
        if (from < to) out.append(src.substr(from, to - from));
    }
    return out;
}

Selection Document::word_around(Location loc) const {
    const Block* b = find(loc.block_id);
    if (b == nullptr) return {loc, loc};
    const std::string_view s = b->source;
    const size_t p = std::min(loc.byte_in_block, s.size());
    if (p >= s.size() || !word_byte(s[p])) return {loc, loc};
    size_t q = p;
    while (q > 0 && word_byte(s[q - 1])) --q;
    size_t e = p;
    while (e < s.size() && word_byte(s[e])) ++e;
    return {{loc.block_id, q}, {loc.block_id, prev_char(s, q, e)}};
}

Selection Document::line_around(Location loc) const {
    const Block* b = find(loc.block_id);
    if (b == nullptr) return {loc, loc};
    const std::string_view s = b->source;
    const size_t p = std::min(loc.byte_in_block, s.size());
    const size_t nl = p > 0 ? s.rfind('\n', p - 1) : std::string_view::npos;
    const size_t q = nl == std::string_view::npos ? 0 : nl + 1;
    size_t e = s.find('\n', p);
    if (e == std::string_view::npos) e = s.size();
    if (e > q && s[e - 1] == '\r') --e;
    return {{loc.block_id, q}, {loc.block_id, prev_char(s, q, e)}};
}

// ---- 渲染器注册 ----

void Document::set_renderer(BlockKind kind, std::unique_ptr<BlockRenderer> r) {
    renderers_[static_cast<size_t>(kind)] = std::move(r);
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

/// @brief 文档版本变化也视为脏。
bool Scrollback::dirty_tree() const noexcept {
    return Widget::dirty_tree() || rendered_rev_ != doc_.revision();
}

size_t Scrollback::max_top() const noexcept {
    const size_t h = static_cast<size_t>(view_ > 0 ? view_ : 0);
    return total_ > h ? total_ - h : 0;
}

void Scrollback::anchor_to(size_t row, int dir) {
    const size_t max = max_top();
    if (row >= max) {
        doc_.set_anchor(Anchor{}); // pinned_to_bottom = true
        return;
    }
    Location loc = doc_.location_of(row, theme_);
    if (dir < 0) {
        // 边距行映射到下方块首：回退到真实内容行
        while (row > 0 &&
               doc_.row_of(loc.block_id, loc.byte_in_block, theme_).value_or(row) > row) {
            loc = doc_.location_of(--row, theme_);
        }
    }
    doc_.set_anchor(Anchor{loc.block_id, loc.byte_in_block, false});
}

void Scrollback::scroll_lines(int lines) {
    if (lines == 0) return;
    const long long max = static_cast<long long>(max_top());
    const long long target = static_cast<long long>(top_) + lines;
    anchor_to(static_cast<size_t>(target < 0 ? 0 : (target > max ? max : target)),
              lines);
    invalidate();
}

void Scrollback::scroll_pages(int pages) {
    if (pages == 0) return;
    scroll_lines(pages * (view_ > 0 ? view_ : 1));
}

void Scrollback::scroll_home() {
    anchor_to(0, 1);
    invalidate();
}

void Scrollback::scroll_end() {
    doc_.set_anchor(Anchor{}); // pinned_to_bottom = true
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
    const Anchor anchor = doc_.anchor();
    if (!anchor.pinned_to_bottom) {
        // 锚点换算视口顶行
        const auto r = doc_.row_of(anchor.block_id, anchor.byte_in_block, theme_);
        top = std::min(r.value_or(0), max);
        // 被夹取时锚点跟随实际顶行。
        if ((!r || top != *r) && total > 0) {
            const Location loc = doc_.location_of(top, theme_);
            doc_.set_anchor(Anchor{loc.block_id, loc.byte_in_block, false});
        }
    }

    doc_.materialize_range(top, view, theme_);

    s.fill({0, 0, w, h}, U' ', theme_.background);
    for (int y = 0; y < h; ++y) {
        const size_t row = top + static_cast<size_t>(y);
        const Line* ln = doc_.line_at(row);
        if (ln != nullptr) draw_row(s, y, row, *ln);
    }
    doc_.evict_outside(top, view);

    total_ = total;
    top_ = top;
    view_ = h;
    unseen_ = total > top + view ? total - top - view : 0;
    rendered_rev_ = doc_.revision();
}

/// @brief 绘制一行；落在选区内的字素套选区样式。
void Scrollback::draw_row(Surface& s, int y, size_t row, const Line& ln) {
    int col = 0;
    const Block* blk = nullptr;
    Location lo;
    Location hi;
    if (selection_) {
        lo = std::min(selection_->anchor, selection_->head);
        hi = std::max(selection_->anchor, selection_->head);
        const uint64_t id = doc_.location_of(row, theme_).block_id;
        if (id >= lo.block_id && id <= hi.block_id) blk = doc_.find(id);
    }
    if (blk == nullptr) {
        for (const Span& sp : ln.spans) col = s.text(col, y, sp.text, sp.style);
    } else {
        walk_line(ln, blk->source,
                  [&](int, int, size_t byte, bool, std::string_view g, const Span& sp) {
                      const Location at{blk->id, byte};
                      Style st = sp.style;
                      if (!(at < lo) && !(hi < at)) {
                          // 选区令牌：设了 fg/bg 就覆盖，其余属性取并集。
                          if (theme_.selection.fg.kind != Color::Kind::default_) {
                              st.fg = theme_.selection.fg;
                          }
                          if (theme_.selection.bg.kind != Color::Kind::default_) {
                              st.bg = theme_.selection.bg;
                          }
                          // 反色取翻转而非并集。
                          if (any(theme_.selection.attrs & Attr::reverse)) {
                              st.attrs = any(st.attrs & Attr::reverse)
                                             ? st.attrs & ~Attr::reverse
                                             : st.attrs | Attr::reverse;
                          }
                          st.attrs = st.attrs | (theme_.selection.attrs & ~Attr::reverse);
                      }
                      col = s.text(col, y, g, st);
                  });
    }
    if (col < s.cols()) {
        s.fill({col, y, s.cols() - col, 1}, U' ', theme_.background);
    }
}

std::optional<Location> Scrollback::hit(Point p) {
    if (total_ == 0 || view_ <= 0) return std::nullopt;
    int x = std::max(p.x, 0);
    if (p.y < 0) x = 0;                                 // 拖出视口上方：到行首
    if (p.y >= view_) x = std::numeric_limits<int>::max(); // 下方：到行尾
    const int y = std::clamp(p.y, 0, view_ - 1);
    size_t row = top_ + static_cast<size_t>(y);
    if (row >= total_) {
        row = total_ - 1; // 视口下方的空白：视为最后一行的行尾之后
        x = std::numeric_limits<int>::max();
    }
    return doc_.location_at(row, x, theme_);
}

void Scrollback::select(Selection sel) {
    selection_ = sel;
    invalidate();
}

void Scrollback::clear_selection() {
    if (!selection_) return;
    selection_.reset();
    invalidate();
}

std::string Scrollback::selected_text() const {
    return selection_ ? doc_.text_between(selection_->anchor, selection_->head)
                      : std::string{};
}

} // namespace dagent::tui
