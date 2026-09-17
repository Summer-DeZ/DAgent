#include <boost/test/unit_test.hpp>

#include <random>

#include "support.hpp"
#include "tui/document.hpp"

using namespace dagent::tui;
using test_support::row_text;

namespace {

// 按宽度走一遍帧路径（计数 + 全量物化），取出每一行的文本、行首偏移、样式。
struct Rows {
    std::vector<std::string> text;
    std::vector<size_t> offset;
    std::vector<Style> style;
};

Rows rows(Document& d, int width, const Theme& theme = {}) {
    d.begin_frame(width, theme.epoch);
    d.materialize_range(0, d.total_rows(), theme);
    Rows out;
    for (size_t r = 0; r < d.total_rows(); ++r) {
        const Line* ln = d.line_at(r);
        BOOST_REQUIRE(ln != nullptr);
        std::string s;
        for (const Span& sp : ln->spans) s += sp.text;
        out.text.push_back(s);
        out.offset.push_back(ln->offset);
        out.style.push_back(ln->spans.empty() ? Style{} : ln->spans.front().style);
    }
    return out;
}

Rows rows_of(std::string_view source, int width, BlockKind kind = BlockKind::text,
             const Theme& theme = {}) {
    Document d;
    d.append_block(kind, std::string(source));
    return rows(d, width, theme);
}

using Strings = std::vector<std::string>;
using Offsets = std::vector<size_t>;

std::string rtrim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// 视口每一行的可见文本（去掉行尾空白）。
Strings screen(const Surface& s) {
    Strings out;
    for (int r = 0; r < s.rows(); ++r) out.push_back(rtrim(row_text(s, r)));
    return out;
}

std::string numbered_lines(int from, int to) {
    std::string s;
    for (int i = from; i <= to; ++i) {
        if (!s.empty()) s += '\n';
        s += std::to_string(i);
    }
    return s;
}

// 每个逻辑行渲染成「原文 + 分隔行」两行：行数与标准折行不同的自定义渲染器。
class RuledRenderer final : public BlockRenderer {
public:
    WrapResult measure(std::string_view source, size_t, int) const override {
        size_t lines = source.empty() ? 0 : 1;
        for (char c : source) lines += c == '\n';
        return {2 * lines, 0, 0};
    }
    size_t render(const Block& b, int, const Theme&, size_t, size_t,
                  std::vector<Line>& out) const override {
        out.clear();
        size_t begin = 0;
        while (begin <= b.source.size() && !b.source.empty()) {
            size_t end = b.source.find('\n', begin);
            if (end == std::string::npos) end = b.source.size();
            out.push_back(Line{{Span{b.source.substr(begin, end - begin), Style{}}}, 0, begin});
            out.push_back(Line{{Span{"--", Style{}}}, 2, begin});
            if (end == b.source.size()) break;
            begin = end + 1;
        }
        return out.size();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(document)

// ---- 折行 ----

BOOST_AUTO_TEST_CASE(wrap_breaks_at_last_space_or_hard_breaks_long_words) {
    Rows r = rows_of("hello world foo", 10);
    BOOST_TEST(r.text == (Strings{"hello ", "world foo"}));
    BOOST_TEST(r.offset == (Offsets{0, 6}));

    r = rows_of("abcdefghijkl", 5);
    BOOST_TEST(r.text == (Strings{"abcde", "fghij", "kl"}));
    BOOST_TEST(r.offset == (Offsets{0, 5, 10}));

    r = rows_of("中文字符", 5);
    BOOST_TEST(r.text == (Strings{"中文", "字符"}));
    BOOST_TEST(r.offset == (Offsets{0, 6}));
}

BOOST_AUTO_TEST_CASE(wrap_expands_tabs_per_row) {
    BOOST_TEST(rows_of("a\tb", 10).text == (Strings{"a       b"}));
    const Rows r = rows_of("a\tb", 8);
    BOOST_TEST(r.text == (Strings{"a       ", "b"}));
    BOOST_TEST(r.offset == (Offsets{0, 2}));
}

BOOST_AUTO_TEST_CASE(wrap_line_terminators) {
    BOOST_TEST(rows_of("a\n\nb", 10).text == (Strings{"a", "", "b"}));
    BOOST_TEST(rows_of("a\n", 10).text == (Strings{"a"}));
    BOOST_TEST(rows_of("", 10).text.empty());
    Rows r = rows_of("a\r\nb", 10);
    BOOST_TEST(r.text == (Strings{"a", "b"}));
    BOOST_TEST(r.offset == (Offsets{0, 3}));
    r = rows_of("a\rb", 10);
    BOOST_TEST(r.text == (Strings{"a", "b"}));
    BOOST_TEST(r.offset == (Offsets{0, 2}));
}

BOOST_AUTO_TEST_CASE(measure_reports_stable_prefix) {
    WrapResult w = wrap_measure_from("ab\ncd", 0, 10);
    BOOST_TEST(w.rows == 2u);
    BOOST_TEST(w.stable_rows == 1u);
    BOOST_TEST(w.stable_bytes == 3u);

    w = wrap_measure_from("abcdefg", 0, 3);  // 超宽断行已定，尾行不定
    BOOST_TEST(w.rows == 3u);
    BOOST_TEST(w.stable_rows == 2u);
    BOOST_TEST(w.stable_bytes == 6u);

    w = wrap_measure_from("xx\nabcdefg", 3, 3);  // stable_bytes 相对扫描起点
    BOOST_TEST(w.rows == 3u);
    BOOST_TEST(w.stable_bytes == 6u);

    w = wrap_measure_from("a\r", 0, 10);  // 末尾的 \r 可能与后续 \n 合并：不定
    BOOST_TEST(w.rows == 1u);
    BOOST_TEST(w.stable_rows == 0u);
    BOOST_TEST(count_rows("hello world foo", 10) == 2u);
}

BOOST_AUTO_TEST_CASE(diff_renderer_styles_whole_logical_lines) {
    Theme th;
    th.add = Style{Color::indexed(2), Color{}, Attr::none};
    th.del = Style{Color::indexed(1), Color{}, Attr::none};
    th.dim = Style{Color::indexed(8), Color{}, Attr::none};
    const Rows r = rows_of("@@ hunk\n+added text\n-gone\nctx", 6, BlockKind::diff, th);
    BOOST_TEST(r.text == (Strings{"@@ ", "hunk", "+added", " text", "-gone", "ctx"}));
    BOOST_TEST(r.style == (std::vector<Style>{th.dim, th.dim, th.add, th.add, th.del, th.text}));
}

// ---- 流式增量 ----

BOOST_AUTO_TEST_CASE(crlf_split_across_appends_matches_one_shot) {
    Document d;
    const uint64_t id = d.open_block(BlockKind::output);
    d.append(id, "a\r");
    rows(d, 20);
    d.append(id, "\nb");
    BOOST_TEST(rows(d, 20).text == (Strings{"a", "b"}));
}

BOOST_AUTO_TEST_CASE(random_streaming_matches_one_shot_wrapping) {
    const Strings atoms = {"a", "word", " ", "  ", "\t", "\n", "\r\n", "\r", "中文",
                           "e\U00000301", "\U00000301", "\U0001F468\U0000200D\U0001F469",
                           "\U0001F1E8\U0001F1F3", "+", "-"};
    Theme th;
    th.add = Style{Color::indexed(2), Color{}, Attr::none};
    th.del = Style{Color::indexed(1), Color{}, Attr::none};
    std::mt19937 rng(515);
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    for (int trial = 0; trial < 400; ++trial) {
        std::string text;
        for (int k = pick(0, 60); k > 0; --k) text += atoms[static_cast<size_t>(pick(0, 14))];
        const int width = pick(2, 12);
        const BlockKind kind = pick(0, 1) ? BlockKind::text : BlockKind::diff;

        // 在任意字节处切块（包括 UTF-8 序列与 \r\n 中间），块间随机出帧
        Document streamed;
        const uint64_t id = streamed.open_block(kind);
        for (size_t pos = 0; pos < text.size();) {
            const size_t n = std::min<size_t>(static_cast<size_t>(pick(1, 7)), text.size() - pos);
            streamed.append(id, std::string_view(text).substr(pos, n));
            pos += n;
            if (pick(0, 1)) rows(streamed, width, th);
        }
        Document whole;
        whole.append_block(kind, text);

        const Rows a = rows(streamed, width, th);
        const Rows b = rows(whole, width, th);
        BOOST_REQUIRE_MESSAGE(a.text == b.text && a.offset == b.offset && a.style == b.style,
                              "trial " << trial << " width " << width);
        streamed.close_block(id);
        BOOST_REQUIRE(rows(streamed, width, th).text == b.text);
    }
}

BOOST_AUTO_TEST_CASE(append_only_accepts_open_blocks) {
    Document d;
    const uint64_t closed = d.append_block(BlockKind::text, "x");
    const uint64_t open = d.open_block(BlockKind::text, "y");
    BOOST_TEST(!d.append(closed, "z"));
    BOOST_TEST(d.append(open, "z"));
    BOOST_TEST(d.close_block(open));
    BOOST_TEST(!d.append(open, "z"));
    BOOST_TEST(!d.append(9999, "z"));
    BOOST_TEST(rows(d, 10).text == (Strings{"x", "yz"}));
}

// ---- 折叠 / 定位 / 裁剪 / 驱逐 ----

BOOST_AUTO_TEST_CASE(collapsed_block_shows_leading_rows_only) {
    Document d;
    const uint64_t id = d.append_block(BlockKind::text, numbered_lines(1, 5));
    d.append_block(BlockKind::text, "tail");
    d.set_collapsed(id, true, 2);
    BOOST_TEST(rows(d, 10).text == (Strings{"1", "2", "tail"}));
    d.set_collapsed(id, false, 2);
    BOOST_TEST(rows(d, 10).text == (Strings{"1", "2", "3", "4", "5", "tail"}));
}

BOOST_AUTO_TEST_CASE(rows_map_to_blocks_and_byte_offsets) {
    const Theme th;
    Document d;
    const uint64_t a = d.append_block(BlockKind::text, "one\ntwo");
    const uint64_t b = d.append_block(BlockKind::text, "");
    const uint64_t c = d.append_block(BlockKind::text, "three");
    const uint64_t e = d.append_block(BlockKind::text, "aaaaaaaaaaaaaaa");
    d.begin_frame(10, th.epoch);
    BOOST_TEST(d.total_rows() == 5u);

    const std::vector<std::pair<uint64_t, size_t>> expected = {{a, 0}, {a, 4}, {c, 0}, {e, 0}, {e, 10}};
    for (size_t row = 0; row < expected.size(); ++row) {
        const Location loc = d.location_of(row, th);
        BOOST_TEST(loc.block_id == expected[row].first);
        BOOST_TEST(loc.byte_in_block == expected[row].second);
    }
    BOOST_TEST(d.row_of(a, 5, th).value() == 1u);   // "two" 中间的字节
    BOOST_TEST(d.row_of(e, 12, th).value() == 4u);
    BOOST_TEST(d.row_of(b, 0, th).value() == 2u);   // 空块位于下一块的起始行

    d.trim_blocks(2);
    BOOST_TEST(d.total_rows() == 3u);
    BOOST_TEST(!d.row_of(a, 0, th).has_value());
    BOOST_TEST(d.location_of(0, th).block_id == c);
    BOOST_TEST(d.row_of(e, 12, th).value() == 2u);
}

BOOST_AUTO_TEST_CASE(trim_rows_keeps_at_least_requested_rows) {
    Document d;
    for (int i = 0; i < 3; ++i) d.append_block(BlockKind::text, "x\ny");
    d.begin_frame(10, 0);
    d.trim_rows(3);
    BOOST_TEST(d.block_count() == 2u);
    BOOST_TEST(d.total_rows() == 4u);
}

BOOST_AUTO_TEST_CASE(eviction_frees_rows_outside_window_but_keeps_open_block) {
    const Theme th;
    Document d;
    d.append_block(BlockKind::text, numbered_lines(0, 9));
    d.append_block(BlockKind::text, numbered_lines(10, 19));
    const uint64_t open = d.open_block(BlockKind::text, numbered_lines(20, 29));
    d.begin_frame(10, th.epoch);
    d.materialize_range(0, d.total_rows(), th);
    d.evict_outside(0, 5);
    BOOST_TEST(d.line_at(0) != nullptr);
    BOOST_TEST(d.line_at(15) == nullptr);
    BOOST_TEST(d.line_at(25) != nullptr);
    BOOST_TEST(d.find(open)->rows_valid == 10u);
}

BOOST_AUTO_TEST_CASE(custom_renderer_row_count_drives_document) {
    Document d;
    d.set_renderer(BlockKind::code, std::make_unique<RuledRenderer>());
    const uint64_t id = d.open_block(BlockKind::code, "one\ntwo");
    BOOST_TEST(rows(d, 20).text == (Strings{"one", "--", "two", "--"}));
    d.append(id, "\nthree");  // 不提供稳定前缀的渲染器：增量路径退化为从头计数
    BOOST_TEST(rows(d, 20).text == (Strings{"one", "--", "two", "--", "three", "--"}));

    Scrollback sb;
    sb.document().set_renderer(BlockKind::code, std::make_unique<RuledRenderer>());
    sb.document().append_block(BlockKind::code, "a\nb");
    Surface s(6, 5);
    sb.render(s);
    BOOST_TEST(screen(s) == (Strings{"a", "--", "b", "--", ""}));
}

// ---- Scrollback ----

BOOST_AUTO_TEST_CASE(pinned_view_follows_new_content) {
    Scrollback sb;
    const uint64_t id = sb.document().open_block(BlockKind::text, numbered_lines(1, 20));
    Surface s(10, 5);
    sb.render(s);
    BOOST_TEST(screen(s) == (Strings{"16", "17", "18", "19", "20"}));
    sb.clear_dirty();
    BOOST_TEST(!sb.dirty_tree());
    sb.document().append(id, "\n21");
    BOOST_TEST(sb.dirty_tree());  // 内容版本变化即脏，无需手动 invalidate
    sb.render(s);
    BOOST_TEST(screen(s) == (Strings{"17", "18", "19", "20", "21"}));
    BOOST_TEST(sb.pinned());
    BOOST_TEST(sb.unseen_rows() == 0u);
}

BOOST_AUTO_TEST_CASE(unpinned_view_holds_position_and_counts_unseen_rows) {
    Scrollback sb;
    const uint64_t id = sb.document().open_block(BlockKind::text, numbered_lines(1, 21));
    Surface s(10, 5);
    sb.render(s);
    sb.scroll_lines(-3);
    BOOST_TEST(!sb.pinned());
    sb.render(s);
    BOOST_TEST(screen(s) == (Strings{"14", "15", "16", "17", "18"}));

    sb.document().append(id, "\n22\n23");
    sb.render(s);
    BOOST_TEST(screen(s) == (Strings{"14", "15", "16", "17", "18"}));
    BOOST_TEST(sb.unseen_rows() == 5u);

    sb.scroll_end();
    sb.render(s);
    BOOST_TEST(sb.pinned());
    BOOST_TEST(screen(s) == (Strings{"19", "20", "21", "22", "23"}));
}

BOOST_AUTO_TEST_CASE(scroll_home_pages_and_bottom_repins) {
    Scrollback sb;
    sb.document().append_block(BlockKind::text, numbered_lines(1, 20));
    Surface s(10, 5);
    sb.render(s);
    sb.scroll_home();
    sb.render(s);
    BOOST_TEST(screen(s).front() == "1");
    sb.scroll_pages(1);
    sb.render(s);
    BOOST_TEST(screen(s).front() == "6");
    sb.scroll_pages(-1);
    sb.render(s);
    BOOST_TEST(screen(s).front() == "1");
    sb.scroll_lines(1000);  // 滚到底即重新贴底
    BOOST_TEST(sb.pinned());
}

BOOST_AUTO_TEST_CASE(anchor_survives_width_changes) {
    Scrollback sb;
    std::string text;
    for (int i = 0; i < 100; ++i) {
        text += "line " + std::string(i < 10 ? "0" : "") + std::to_string(i) +
                " xxxxxxxxxxxxxxxxxxxxxx\n";
    }
    sb.document().append_block(BlockKind::text, text);
    Surface wide(40, 10);
    sb.render(wide);
    sb.scroll_home();
    sb.render(wide);
    sb.scroll_lines(50);
    sb.render(wide);
    BOOST_TEST(screen(wide).front() == "line 50 xxxxxxxxxxxxxxxxxxxxxx");

    Surface narrow(15, 10);
    sb.render(narrow);
    BOOST_TEST(screen(narrow).front() == "line 50");
    sb.render(wide);
    BOOST_TEST(screen(wide).front() == "line 50 xxxxxxxxxxxxxxxxxxxxxx");
}

BOOST_AUTO_TEST_CASE(anchor_in_trimmed_history_clamps_to_oldest_row) {
    Scrollback sb;
    for (int i = 0; i < 30; ++i) {
        sb.document().append_block(BlockKind::text, "b" + std::string(i < 10 ? "0" : "") + std::to_string(i));
    }
    Surface s(6, 5);
    sb.render(s);
    sb.scroll_home();
    sb.render(s);
    sb.scroll_lines(2);
    sb.render(s);
    BOOST_TEST(screen(s).front() == "b02");

    sb.document().trim_blocks(20);
    sb.render(s);
    BOOST_TEST(!sb.pinned());
    BOOST_TEST(screen(s).front() == "b10");
    sb.document().append_block(BlockKind::text, "b30");
    sb.render(s);
    BOOST_TEST(screen(s).front() == "b10");  // 锚点已跟随到现存最旧行
}

BOOST_AUTO_TEST_CASE(streaming_into_chat_screen_replays_exactly) {
    Container root;
    auto sb_owner = std::make_unique<Scrollback>();
    auto input_owner = std::make_unique<InputBox>();
    Scrollback& sb = *sb_owner;
    root.add({Sizing::flex, 1}, std::move(sb_owner));
    root.add({Sizing::content, 0, 3, 6}, std::move(input_owner));

    Document& doc = sb.document();
    for (int i = 0; i < 200; ++i) doc.append_block(BlockKind::text, "history line " + std::to_string(i));
    const uint64_t id = doc.open_block(BlockKind::text);

    Surface front, back;
    test_support::VtScreen vt(60, 15);
    std::string out;
    auto frame = [&] {
        back.resize(60, 15);
        back.copy_from(front);
        if (root.needs_layout() || !(root.rect() == Rect{0, 0, 60, 15})) root.layout({0, 0, 60, 15});
        root.render(back);
        render_frame(out, back, front, {});
        vt.feed(out);
        std::swap(front, back);
        back.clear_dirty();
    };
    frame();

    int same_row_frames = 0;
    for (int t = 0; t < 120; ++t) {
        const size_t rows_before = doc.total_rows();
        doc.append(id, t % 17 == 16 ? "\n" : "tok ");
        frame();
        BOOST_REQUIRE_MESSAGE(vt.errors().empty(), vt.errors().front());
        BOOST_REQUIRE_MESSAGE(test_support::screen_mismatch(vt, front).empty(),
                              "token " << t << ": " << test_support::screen_mismatch(vt, front));
        if (doc.total_rows() == rows_before) {
            // 没有新增行：只重写滚动区最后一行（第 12 行），字节量很小
            ++same_row_frames;
            for (int row : test_support::cup_rows(out)) BOOST_TEST(row == 12);
            BOOST_TEST(out.size() < 200u);
        }
    }
    BOOST_TEST(same_row_frames > 50);
}

BOOST_AUTO_TEST_SUITE_END()
