#include <boost/test/unit_test.hpp>

#include <random>

#include "support.hpp"
#include "tui/surface.hpp"

using namespace dagent::tui;
using test_support::row_text;

namespace {

// 网格不变量：width==2 后面紧跟同字素、同样式的 width==0 占位格；
// 不存在孤立的 width==0 格。
std::string invariant_violation(const Surface& s) {
    for (int r = 0; r < s.rows(); ++r) {
        for (int c = 0; c < s.cols(); ++c) {
            const Cell& cell = s.at(c, r);
            const std::string where = " at (" + std::to_string(c) + "," + std::to_string(r) + ")";
            if (cell.width == 2) {
                if (c + 1 >= s.cols()) return "wide cell in last column" + where;
                const Cell& right = s.at(c + 1, r);
                if (right.width != 0 || right.grapheme() != cell.grapheme() ||
                    !(right.style == cell.style)) {
                    return "wide cell without matching placeholder" + where;
                }
            } else if (cell.width == 0) {
                if (c == 0 || s.at(c - 1, r).width != 2) return "orphan placeholder" + where;
            }
        }
    }
    return {};
}

const Style kRed{Color::indexed(1), Color{}, Attr::none};

} // namespace

BOOST_AUTO_TEST_SUITE(surface)

BOOST_AUTO_TEST_CASE(resize_allocates_blank_and_marks_all_rows_dirty) {
    Surface s(4, 3);
    s.clear_dirty();
    s.put(1, 1, "x", Style{});
    s.resize(4, 3);  // 同尺寸：内容保留
    BOOST_TEST(s.at(1, 1).grapheme() == "x");
    s.resize(5, 2);
    BOOST_TEST(row_text(s, 0) == "     ");
    BOOST_TEST(row_text(s, 1) == "     ");
    BOOST_TEST((s.row_dirty(0) && s.row_dirty(1)));
}

BOOST_AUTO_TEST_CASE(out_of_bounds_access_is_clipped) {
    Surface s(3, 2);
    s.clear_dirty();
    s.put(-1, 0, "x", Style{});
    s.put(3, 0, "x", Style{});
    s.put(0, 2, "x", Style{});
    s.fill({-5, -5, 2, 2}, U'x', Style{});
    BOOST_TEST(row_text(s, 0) == "   ");
    BOOST_TEST(row_text(s, 1) == "   ");
    BOOST_TEST((!s.row_dirty(0) && !s.row_dirty(1)));
    BOOST_TEST(s.at(99, 99).grapheme() == " ");
}

BOOST_AUTO_TEST_CASE(put_marks_only_its_row_dirty) {
    Surface s(4, 3);
    s.clear_dirty();
    s.put(2, 1, "a", kRed);
    BOOST_TEST(!s.row_dirty(0));
    BOOST_TEST(s.row_dirty(1));
    BOOST_TEST(!s.row_dirty(2));
    BOOST_TEST(s.at(2, 1).style == kRed);
}

BOOST_AUTO_TEST_CASE(wide_char_occupies_two_cells_and_degrades_at_edge) {
    Surface s(4, 1);
    s.put(0, 0, "中", Style{});
    BOOST_TEST(s.at(0, 0).width == 2);
    BOOST_TEST(s.at(1, 0).width == 0);
    s.put(3, 0, "中", Style{});  // 最后一列放不下
    BOOST_TEST(s.at(3, 0).grapheme() == " ");
    BOOST_TEST(s.at(3, 0).width == 1);
}

BOOST_AUTO_TEST_CASE(overwriting_half_of_wide_char_blanks_the_other_half) {
    Surface s(6, 1);
    s.text(0, 0, "中文字", Style{});
    s.put(1, 0, "a", Style{});  // 覆盖"中"的右半
    BOOST_TEST(row_text(s, 0) == " a文字");
    s.put(2, 0, "b", Style{});  // 覆盖"文"的左半
    BOOST_TEST(row_text(s, 0) == " ab 字");
    s.put(3, 0, "中", Style{});  // 宽字写在"字"左邻的空格 + "字"左半上
    BOOST_TEST(row_text(s, 0) == " ab中 ");
    BOOST_TEST(invariant_violation(s) == "");
}

BOOST_AUTO_TEST_CASE(long_cluster_is_interned_and_roundtrips) {
    const std::string family = "\U0001F468\U0000200D\U0001F469\U0000200D\U0001F467";
    Surface s(4, 1);
    s.put(0, 0, family, Style{});
    s.put(2, 0, family, Style{});
    BOOST_TEST(s.at(0, 0).grapheme() == family);
    BOOST_TEST(s.at(0, 0).width == 2);
    BOOST_TEST((s.at(0, 0) == s.at(2, 0)));  // 同一簇复用同一 intern 索引
}

BOOST_AUTO_TEST_CASE(text_advances_by_display_width) {
    Surface s(12, 1);
    const int end = s.text(0, 0, "a中e\U00000301\U0000200Bb", Style{});
    BOOST_TEST(end == 5);  // a(1) 中(2) é(1) 零宽(0) b(1)
    BOOST_TEST(row_text(s, 0) == "a中e\U00000301b       ");
}

BOOST_AUTO_TEST_CASE(text_stops_at_newline_and_drops_controls) {
    Surface s(8, 1);
    BOOST_TEST(s.text(0, 0, "a\x01" "b\nc", Style{}) == 2);
    BOOST_TEST(row_text(s, 0) == "ab      ");
    Surface t(8, 1);
    BOOST_TEST(t.text(0, 0, "x\ry", Style{}) == 1);
}

BOOST_AUTO_TEST_CASE(text_wide_char_that_does_not_fit_stops_writing) {
    Surface s(4, 1);
    BOOST_TEST(s.text(0, 0, "ab中x", Style{}) == 4);
    BOOST_TEST(row_text(s, 0) == "ab中");
    Surface t(4, 1);
    BOOST_TEST(t.text(0, 0, "abc中x", Style{}) == 3);
    BOOST_TEST(row_text(t, 0) == "abc ");
}

BOOST_AUTO_TEST_CASE(text_tab_stops_are_relative_to_origin) {
    Surface s(12, 1);
    BOOST_TEST(s.text(0, 0, "a\tb", Style{}) == 9);
    BOOST_TEST(s.at(8, 0).grapheme() == "b");

    Surface t(12, 1);
    BOOST_TEST(t.text(2, 0, "a\tb", Style{}) == 11);
    BOOST_TEST(t.at(10, 0).grapheme() == "b");
}

BOOST_AUTO_TEST_CASE(text_with_negative_origin_clips_left_part) {
    Surface s(10, 1);
    s.text(-3, 0, "a\tb", Style{});
    BOOST_TEST(row_text(s, 0) == "     b    ");

    Surface t(6, 1);
    t.text(-1, 0, "中文", Style{});  // "中"跨过第 0 列：可见半格补空格
    BOOST_TEST(row_text(t, 0) == " 文   ");
    BOOST_TEST(invariant_violation(t) == "");
}

BOOST_AUTO_TEST_CASE(fill_steps_by_glyph_width) {
    Surface s(5, 2);
    s.fill({0, 0, 5, 2}, U'中', kRed);
    BOOST_TEST(row_text(s, 0) == "中中 ");  // 奇数宽度：末列降级为空格
    BOOST_TEST(row_text(s, 1) == "中中 ");
    BOOST_TEST(s.at(4, 0).style == kRed);
    BOOST_TEST(invariant_violation(s) == "");

    Surface z(3, 1);
    z.clear_dirty();
    z.fill({0, 0, 3, 1}, U'\U00000301', Style{});  // 零宽字符不留格
    BOOST_TEST(row_text(z, 0) == "   ");
    BOOST_TEST(!z.row_dirty(0));
}

BOOST_AUTO_TEST_CASE(hline_is_inclusive_and_clipped) {
    Surface s(6, 2);
    s.hline(1, 2, 4, Style{});
    BOOST_TEST(row_text(s, 1) == "  ─── ");
    s.hline(0, -3, 99, Style{});
    BOOST_TEST(row_text(s, 0) == "──────");
}

BOOST_AUTO_TEST_CASE(view_translates_clips_and_writes_dirty_through) {
    Surface host(8, 4);
    host.clear_dirty();
    Surface v = host.view({2, 1, 4, 2});
    BOOST_TEST(v.cols() == 4);
    BOOST_TEST(v.rows() == 2);
    v.fill({-10, -10, 100, 100}, U'x', Style{});  // 越界填充被裁剪在视图内
    BOOST_TEST(row_text(host, 0) == "        ");
    BOOST_TEST(row_text(host, 1) == "  xxxx  ");
    BOOST_TEST(row_text(host, 2) == "  xxxx  ");
    BOOST_TEST(row_text(host, 3) == "        ");
    BOOST_TEST((!host.row_dirty(0) && host.row_dirty(1) && host.row_dirty(2) && !host.row_dirty(3)));

    Surface nested = v.view({1, 1, 2, 1});
    nested.put(0, 0, "y", Style{});
    BOOST_TEST(host.at(3, 2).grapheme() == "y");

    v.text(1, 0, "中文", Style{});  // "文"在视图右缘放不下：停止，不跨入宿主相邻列
    BOOST_TEST(row_text(host, 1) == "  x中x  ");
}

BOOST_AUTO_TEST_CASE(copy_from_matches_source_and_clears_dirty) {
    Surface a(4, 2);
    a.text(0, 1, "ab中", kRed);
    Surface b(4, 2);
    b.copy_from(a);
    BOOST_TEST(row_text(b, 1) == "ab中");
    BOOST_TEST((!b.row_dirty(0) && !b.row_dirty(1)));

    Surface c(3, 2);
    c.put(0, 0, "z", Style{});
    c.copy_from(a);  // 尺寸不一致：不复制
    BOOST_TEST(c.at(0, 0).grapheme() == "z");
}

BOOST_AUTO_TEST_CASE(random_drawing_preserves_grid_invariant) {
    const std::vector<std::string> glyphs = {
        "a", " ", "中", "e\U00000301", "\U0001F1E8\U0001F1F3",
        "\U0001F468\U0000200D\U0001F469\U0000200D\U0001F467", "\t", "字x中",
    };
    std::mt19937 rng(20260917);
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    Surface host(13, 5);
    for (int step = 0; step < 5000; ++step) {
        Rect vr{pick(0, 12), pick(0, 4), pick(1, 13), pick(1, 5)};
        Surface v = pick(0, 1) ? host.view(vr) : host.view({0, 0, 13, 5});
        const std::string& g = glyphs[static_cast<std::size_t>(pick(0, 7))];
        switch (pick(0, 3)) {
        case 0: v.put(pick(-2, 14), pick(-1, 5), g, Style{}); break;
        case 1: v.text(pick(-4, 14), pick(-1, 5), g, Style{}); break;
        case 2: v.fill({pick(-2, 12), pick(-2, 4), pick(0, 8), pick(0, 3)},
                       pick(0, 1) ? U'中' : U'.', Style{}); break;
        case 3: v.hline(pick(-1, 5), pick(-2, 12), pick(-2, 14), Style{}); break;
        }
        const std::string bad = invariant_violation(host);
        BOOST_REQUIRE_MESSAGE(bad.empty(), "step " << step << ": " << bad);
    }
}

BOOST_AUTO_TEST_SUITE_END()
