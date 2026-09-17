#include <boost/test/unit_test.hpp>

#include "support.hpp"
#include "tui/widget.hpp"

using namespace dagent::tui;
using test_support::row_text;

namespace {

// 让控件占满 w×h 并画一帧（控件自身坐标系）。
Surface draw(Widget& w, int cols, int rows) {
    w.layout({0, 0, cols, rows});
    Surface s(cols, rows);
    s.fill({0, 0, cols, rows}, U'Z', Style{});  // 残留内容：render 必须自行清底
    w.render(s);
    return s;
}

std::string with_caret(InputBox& box) {
    box.insert("|");
    return box.text();
}

} // namespace

BOOST_AUTO_TEST_SUITE(widget)

// ---- Text ----

BOOST_AUTO_TEST_CASE(text_measures_lines_and_widest_display_width) {
    Text t;
    t.set_text("ab\n中文\tx");
    BOOST_TEST((t.measure({}) == Size{9, 2}));
    const Surface s = draw(t, 12, 3);
    BOOST_TEST(row_text(s, 0) == "ab          ");
    BOOST_TEST(row_text(s, 1) == "中文    x   ");
    BOOST_TEST(row_text(s, 2) == "            ");
}

BOOST_AUTO_TEST_CASE(text_style_change_only_redraws) {
    Text t;
    t.set_text("x");
    t.layout({0, 0, 3, 1});
    t.clear_dirty();
    const Style red{Color::indexed(1), Color{}, Attr::none};
    t.set_style(red);
    BOOST_TEST(t.dirty());
    BOOST_TEST(!t.needs_layout());
    const Surface s = draw(t, 3, 1);
    BOOST_TEST(s.at(0, 0).style == red);

    t.set_text("y\nz");
    BOOST_TEST(t.needs_layout());
}

// ---- Activity ----

BOOST_AUTO_TEST_CASE(activity_is_zero_height_when_idle) {
    Activity a;
    BOOST_TEST((a.measure({}) == Size{0, 0}));
    a.set_action("读取 a.txt");
    BOOST_TEST(a.needs_layout());
    BOOST_TEST((a.measure({}) == Size{12, 1}));
    a.set_action("");
    BOOST_TEST((a.measure({}) == Size{0, 0}));
}

BOOST_AUTO_TEST_CASE(activity_tick_advances_spinner_without_layout) {
    Activity a;
    a.set_action("读取 a.txt");
    Surface s = draw(a, 16, 1);
    BOOST_TEST(row_text(s, 0) == "⠋ 读取 a.txt    ");

    a.tick();
    BOOST_TEST(!a.needs_layout());
    BOOST_TEST(a.dirty());
    s = draw(a, 16, 1);
    BOOST_TEST(s.at(0, 0).grapheme() == "⠙");

    for (int i = 0; i < 9; ++i) a.tick();  // 10 帧一个周期
    s = draw(a, 16, 1);
    BOOST_TEST(s.at(0, 0).grapheme() == "⠋");
}

// ---- Notice ----

BOOST_AUTO_TEST_CASE(notice_style_follows_severity) {
    Notice n;
    n.show(Notice::Severity::error, "失败 x");
    BOOST_TEST((n.measure({}) == Size{6, 1}));
    Surface s = draw(n, 8, 1);
    BOOST_TEST(row_text(s, 0) == "失败 x  ");
    BOOST_TEST(s.at(0, 0).style == (Style{Color::indexed(1), Color{}, Attr::bold}));

    n.show(Notice::Severity::warn, "注意");
    s = draw(n, 8, 1);
    BOOST_TEST(s.at(0, 0).style == (Style{Color::indexed(3), Color{}, Attr::none}));

    n.show(Notice::Severity::info, "ok");
    s = draw(n, 8, 1);
    BOOST_TEST(s.at(0, 0).style == Style{});

    n.show(Notice::Severity::info, "");
    BOOST_TEST((n.measure({}) == Size{0, 0}));
}

// ---- InputBox：编辑模型 ----

BOOST_AUTO_TEST_CASE(input_insert_splits_lines_and_places_caret_after_insertion) {
    InputBox box;
    BOOST_TEST((box.measure({}) == Size{2, 3}));
    box.insert("ab\ncd");
    BOOST_TEST(box.text() == "ab\ncd");
    BOOST_TEST((box.measure({}) == Size{4, 4}));

    box.set_text("abcd");
    box.move(2, 0);
    box.insert("X\nY");
    BOOST_TEST(with_caret(box) == "abX\nY|cd");
}

BOOST_AUTO_TEST_CASE(input_deletes_whole_grapheme_clusters) {
    InputBox box;
    box.insert("ae\U00000301\U0001F468\U0000200D\U0001F469");
    box.backspace();
    BOOST_TEST(box.text() == "ae\U00000301");
    box.backspace();
    BOOST_TEST(box.text() == "a");
    box.line_home();
    box.del();
    BOOST_TEST(box.text() == "");
}

BOOST_AUTO_TEST_CASE(input_backspace_and_delete_join_lines) {
    InputBox box;
    box.set_text("ab\ncd");
    box.move(0, 1);
    box.backspace();
    BOOST_TEST(with_caret(box) == "ab|cd");

    box.set_text("ab\ncd");
    box.line_end();
    box.del();
    BOOST_TEST(with_caret(box) == "ab|cd");
}

BOOST_AUTO_TEST_CASE(input_edits_at_boundaries_are_noops) {
    InputBox box;
    box.set_text("a");
    box.backspace();
    box.line_end();
    box.del();
    BOOST_TEST(box.text() == "a");
}

BOOST_AUTO_TEST_CASE(input_horizontal_motion_crosses_lines) {
    InputBox box;
    box.set_text("ab\ncd");
    box.line_end();
    box.move(1, 0);
    BOOST_TEST(with_caret(box) == "ab\n|cd");
    box.move(-2, 0);
    BOOST_TEST(with_caret(box) == "ab|\n|cd");
    box.set_text("x");
    box.move(-5, 0);
    box.move(0, -5);
    BOOST_TEST(with_caret(box) == "|x");
}

BOOST_AUTO_TEST_CASE(input_vertical_motion_keeps_goal_display_column) {
    InputBox box;
    box.set_text("中文字\nabcdef\nx");
    box.move(2, 0);   // 显示列 4
    box.move(0, 1);   // abcd|ef
    box.move(0, 1);   // x|（行短，夹到行尾）
    box.move(0, -2);  // 回到显示列 4，而不是 1
    BOOST_TEST(with_caret(box) == "中文|字\nabcdef\nx");

    box.set_text("abc\n中文");
    box.move(1, 0);  // 显示列 1 落在"中"的两列之间 → 停在字素边界之后
    box.move(0, 1);
    BOOST_TEST(with_caret(box) == "abc\n中|文");
}

BOOST_AUTO_TEST_CASE(input_set_text_resets_caret_and_home_end_work) {
    InputBox box;
    box.set_text("hello\nworld");
    BOOST_TEST(with_caret(box) == "|hello\nworld");
    box.set_text("abc");
    box.line_end();
    box.line_home();
    BOOST_TEST(with_caret(box) == "|abc");
}

BOOST_AUTO_TEST_CASE(input_normalizes_carriage_returns_and_controls) {
    InputBox box;
    box.insert("x\r\ny\x01z\x7f");
    BOOST_TEST(box.text() == "x\nyz");
    box.set_text("a\rb\r\nc\td");
    BOOST_TEST(box.text() == "a\nb\nc\td");
}

BOOST_AUTO_TEST_CASE(input_edits_request_layout_but_motion_only_redraws) {
    InputBox box;
    box.layout({0, 0, 10, 3});
    box.insert("a");
    BOOST_TEST(box.needs_layout());
    box.layout({0, 0, 10, 3});
    box.clear_dirty();
    box.move(-1, 0);
    BOOST_TEST(!box.needs_layout());
    BOOST_TEST(box.dirty());
}

// ---- InputBox：渲染与光标 ----

BOOST_AUTO_TEST_CASE(input_draws_border_and_cursor) {
    InputBox box;
    box.set_text("hi");
    const Surface s = draw(box, 6, 3);
    BOOST_TEST(row_text(s, 0) == "┌────┐");
    BOOST_TEST(row_text(s, 1) == "│hi  │");
    BOOST_TEST(row_text(s, 2) == "└────┘");
    BOOST_TEST((box.cursor() == Point{1, 1}));
}

BOOST_AUTO_TEST_CASE(input_scrolls_vertically_to_keep_cursor_visible) {
    InputBox box;
    box.set_text("1\n2\n3\n4\n5");
    box.move(0, 4);
    Surface s = draw(box, 5, 4);
    BOOST_TEST(row_text(s, 1) == "│4  │");
    BOOST_TEST(row_text(s, 2) == "│5  │");
    BOOST_TEST((box.cursor() == Point{1, 2}));

    box.move(0, -4);
    s = draw(box, 5, 4);
    BOOST_TEST(row_text(s, 1) == "│1  │");
    BOOST_TEST(row_text(s, 2) == "│2  │");
    BOOST_TEST((box.cursor() == Point{1, 1}));
}

BOOST_AUTO_TEST_CASE(input_horizontal_scroll_keeps_real_display_columns) {
    InputBox tab;
    tab.insert("a\tbc");
    Surface s = draw(tab, 10, 3);
    BOOST_TEST(row_text(s, 1) == "│     bc │");
    BOOST_TEST((tab.cursor() == Point{8, 1}));

    InputBox wide;
    wide.insert("中中中中中");
    s = draw(wide, 10, 3);
    BOOST_TEST(row_text(s, 1) == "│ 中中中 │");
    BOOST_TEST((wide.cursor() == Point{8, 1}));

    InputBox lines;  // 所有可见行共用同一水平偏移
    lines.set_text("0123456789\nabcdefghij");
    lines.line_end();
    s = draw(lines, 8, 4);
    BOOST_TEST(row_text(s, 1) == "│56789 │");
    BOOST_TEST(row_text(s, 2) == "│fghij │");
    BOOST_TEST((lines.cursor() == Point{6, 1}));
}

BOOST_AUTO_TEST_CASE(input_too_small_for_border_only_clears) {
    InputBox box;
    box.set_text("abc");
    const Surface s = draw(box, 2, 2);
    BOOST_TEST(row_text(s, 0) == "  ");
    BOOST_TEST(row_text(s, 1) == "  ");
    BOOST_TEST(!box.cursor().has_value());
}

BOOST_AUTO_TEST_SUITE_END()
