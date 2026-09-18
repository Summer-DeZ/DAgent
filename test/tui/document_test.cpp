// §3.8 选择与复制验收（文档层）：选区用逻辑位置（块 id + 字节偏移）
// 表示，复制的是源文本而不是屏幕字符；改变宽度后选区覆盖同一段字节，
// 反色落在同一段内容上。直接驱动真实的 Scrollback + Document，不做模拟。

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

#include "tui/document.hpp"
#include "tui/surface.hpp"

// Boost.Test 断言失败时要打印位置，测试 TU 内补上流输出。
namespace dagent::tui {
inline std::ostream& operator<<(std::ostream& os, const Location& l) {
    return os << "Location{" << l.block_id << "," << l.byte_in_block << "}";
}
} // namespace dagent::tui

using namespace dagent::tui;

namespace {

// 文本块 A（会被软折行）+ 带上边距的 markdown 块 B（行内标记隐藏）。
constexpr std::string_view k_text = "alpha beta gamma delta epsilon";

struct Fixture {
    Scrollback sb;
    uint64_t a = 0;
    uint64_t b = 0;

    Fixture() {
        a = sb.document().append_block(BlockKind::text, std::string(k_text));
        Block md;
        md.kind = BlockKind::markdown;
        md.source = "**bold** tail\n";
        md.margin_top = 1;
        b = sb.document().append_block(std::move(md));
    }

    void render(int width, int height, Surface& s) {
        s.resize(width, height);
        sb.layout({0, 0, width, height});
        sb.render(s);
    }
};

// 反色单元格按行序拼接（选区高亮的屏幕内容）。
std::string highlighted(const Surface& s) {
    std::string out;
    for (int r = 0; r < s.rows(); ++r) {
        for (int c = 0; c < s.cols(); ++c) {
            const Cell& cell = s.at(c, r);
            if (cell.width == 0) continue;
            if (any(cell.style.attrs & Attr::reverse)) out += cell.grapheme();
        }
    }
    return out;
}

} // namespace

BOOST_AUTO_TEST_SUITE(document)

BOOST_AUTO_TEST_CASE(selection_copies_source_text_across_soft_wraps) {
    Fixture f;
    Surface s;
    f.render(12, 6, s);
    // 宽 12：A 折成 "alpha beta " / "gamma delta " / "epsilon"，
    // 空一行（B 的上边距），B 显示为 "bold tail"。
    BOOST_TEST(s.at(0, 1).grapheme() == "g");
    BOOST_TEST(s.at(0, 4).grapheme() == "b");

    // 从 A 的开头拖到 B 显示列 3（"bold" 的 d，源里在 "**" 之后）：
    // 复制出源文本 —— 软折行处没有换行，块间补换行与段落空行，
    // markdown 的标记原样保留。
    const auto from = f.sb.hit({0, 0});
    const auto to = f.sb.hit({3, 4});
    BOOST_REQUIRE(from && to);
    BOOST_TEST(*from == (Location{f.a, 0}));
    BOOST_TEST(*to == (Location{f.b, 5}));
    f.sb.select({*from, *to});
    BOOST_TEST(f.sb.selected_text() == "alpha beta gamma delta epsilon\n\n**bold");

    // 反向拖拽等价；软折行的行尾之后取行内最后一个字素（折行处的空格），
    // 块尾之后取块尾。
    f.sb.select({*to, *from});
    BOOST_TEST(f.sb.selected_text() == "alpha beta gamma delta epsilon\n\n**bold");
    BOOST_TEST(*f.sb.hit({11, 0}) == (Location{f.a, 10}));
    BOOST_TEST(*f.sb.hit({11, 2}) == (Location{f.a, k_text.size()}));

    // 双击选词、三击选逻辑行（不含行尾换行）。
    const Document& d = f.sb.document();
    const Selection word = d.word_around(*f.sb.hit({2, 1}));
    BOOST_TEST(d.text_between(word.anchor, word.head) == "gamma");
    const Selection line = d.line_around(*f.sb.hit({6, 4}));
    BOOST_TEST(d.text_between(line.anchor, line.head) == "**bold** tail");
}

BOOST_AUTO_TEST_CASE(selection_survives_width_change) {
    Fixture f;
    Surface s;
    f.render(12, 6, s);
    // 选中 "beta"（首行列 6）到 "delta" 末字符（第二行列 10）。
    const auto from = f.sb.hit({6, 0});
    const auto to = f.sb.hit({10, 1});
    BOOST_REQUIRE(from && to);
    f.sb.select({*from, *to});
    const std::string want = f.sb.selected_text();
    BOOST_TEST(want == "beta gamma delta");
    f.render(12, 6, s);
    BOOST_TEST(highlighted(s) == want);

    // 改变宽度：选区的字节范围不变，反色仍落在同一段内容上。
    for (const int width : {7, 30, 9}) {
        f.render(width, 8, s);
        BOOST_TEST_CONTEXT("width " << width) {
            BOOST_REQUIRE(f.sb.selection().has_value());
            BOOST_TEST(f.sb.selection()->anchor == *from);
            BOOST_TEST(f.sb.selection()->head == *to);
            BOOST_TEST(f.sb.selected_text() == want);
            BOOST_TEST(highlighted(s) == want);
        }
    }

    // 清除选区后不再有反色。
    f.sb.clear_selection();
    f.render(9, 8, s);
    BOOST_TEST(highlighted(s).empty());
}

BOOST_AUTO_TEST_SUITE_END()
