// §10.10 选择与复制验收（文档层）：选区用逻辑位置（块 id + 字节偏移）
// 表示，复制的是源文本而不是屏幕字符；改变宽度后选区覆盖同一段字节，
// 反色落在同一段内容上。直接驱动真实的 Scrollback + Document，不做模拟。

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "tui/document.hpp"
#include "tui/surface.hpp"

// Boost.Test 断言失败时要打印位置/颜色，测试 TU 内补上流输出。
namespace dagent::tui {
inline std::ostream& operator<<(std::ostream& os, const Location& l) {
    return os << "Location{" << l.block_id << "," << l.byte_in_block << "}";
}
inline std::ostream& operator<<(std::ostream& os, const Color& c) {
    return os << "Color{" << static_cast<int>(c.kind) << ","
              << static_cast<int>(c.r) << "," << static_cast<int>(c.g) << ","
              << static_cast<int>(c.b) << "}";
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

// §9.2 验收：修改令牌并递增 epoch 后，所有块的物化缓存失效、整块重排
// 使用新样式；只改令牌不递增 epoch 则缓存不失效（epoch 是唯一契约）。
BOOST_AUTO_TEST_CASE(theme_epoch_invalidates_all_block_materialization) {
    Scrollback sb;
    Document& d = sb.document();
    d.append_block(BlockKind::text, "plain");
    Block code;
    code.kind = BlockKind::code;
    code.source = "int x;";
    code.meta = "cpp";
    d.append_block(std::move(code));
    d.append_block(BlockKind::diff, "+add");
    d.append_block(BlockKind::markdown, "# h");
    Block table;
    table.kind = BlockKind::table;
    table.source = "| a |\n| --- |\n| b |\n";
    d.append_block(std::move(table));

    // 每套测试主题用不同索引色标出各渲染器引用到的令牌。
    const auto make = [](int text, int keyword, int added, int heading,
                         int border) {
        ThemeTokens t;
        t.text = Style{Color::indexed(static_cast<uint8_t>(text)), Color{},
                       Attr::none};
        t.syntax_keyword =
            Style{Color::indexed(static_cast<uint8_t>(keyword)), Color{},
                  Attr::none};
        t.diff_added = Style{Color::indexed(static_cast<uint8_t>(added)),
                             Color{}, Attr::none};
        t.markdown_heading =
            Style{Color::indexed(static_cast<uint8_t>(heading)), Color{},
                  Attr::none};
        t.primary = t.markdown_heading; // 表头
        t.border = Style{Color::indexed(static_cast<uint8_t>(border)), Color{},
                         Attr::none};
        return t;
    };

    Surface s;
    const auto render = [&](const ThemeTokens& theme) {
        sb.set_theme(theme);
        s.resize(20, 8);
        sb.layout({0, 0, 20, 8});
        sb.render(s);
    };
    const auto fg = [&](int x, int y) { return s.at(x, y).style.fg; };

    ThemeTokens first = make(1, 2, 3, 4, 5);
    first.epoch = 1;
    render(first);
    // 行 0 文本、行 1 代码关键字（列 2：列 0-1 是代码块竖条）、行 2 diff '+'、行 3 标题前缀、
    // 行 4 表头/边框、行 6 表格正文。
    BOOST_TEST(fg(0, 0) == Color::indexed(1));
    BOOST_TEST(fg(2, 1) == Color::indexed(2));
    BOOST_TEST(fg(0, 2) == Color::indexed(3));
    BOOST_TEST(fg(0, 3) == Color::indexed(4));
    BOOST_TEST(fg(0, 4) == Color::indexed(5));
    BOOST_TEST(fg(2, 4) == Color::indexed(4));
    BOOST_TEST(fg(2, 6) == Color::indexed(1));

    // 只改令牌、不递增 epoch：物化缓存不失效，仍是旧样式。
    ThemeTokens stale = make(11, 12, 13, 14, 15);
    stale.epoch = first.epoch;
    render(stale);
    BOOST_TEST(fg(0, 0) == Color::indexed(1));
    BOOST_TEST(fg(2, 1) == Color::indexed(2));

    // 递增 epoch：所有块重排，新样式落到每个渲染器。
    ThemeTokens next = make(11, 12, 13, 14, 15);
    next.epoch = first.epoch + 1;
    render(next);
    BOOST_TEST(fg(0, 0) == Color::indexed(11));
    BOOST_TEST(fg(2, 1) == Color::indexed(12));
    BOOST_TEST(fg(0, 2) == Color::indexed(13));
    BOOST_TEST(fg(0, 3) == Color::indexed(14));
    BOOST_TEST(fg(0, 4) == Color::indexed(15));
    BOOST_TEST(fg(2, 4) == Color::indexed(14));
    BOOST_TEST(fg(2, 6) == Color::indexed(11));
}

// dark / light 选择：按背景相对亮度（阈值 0.5），取不到默认 dark。
BOOST_AUTO_TEST_CASE(theme_selects_dark_or_light_by_background_luminance) {
    const ThemeTokens& dark = dark_theme();
    const ThemeTokens& light = light_theme();
    BOOST_TEST(&default_theme(std::nullopt) == &dark);
    BOOST_TEST(&default_theme(Color::rgb(16, 16, 16)) == &dark);
    BOOST_TEST(&default_theme(Color::rgb(250, 250, 250)) == &light);
    BOOST_TEST(relative_luminance(Color::rgb(0, 0, 0)) == 0.0f);
    BOOST_TEST(relative_luminance(Color::rgb(255, 255, 255)) > 0.9f);
    BOOST_TEST(&default_theme(Color::indexed(1)) == &dark); // 索引色无从判断
}

// 中文折行、表格行内样式、代码块竖条（冻结后的缺陷修复）。
BOOST_AUTO_TEST_CASE(cjk_wrap_table_inline_and_code_gutter) {
    ThemeTokens th = dark_theme();
    th.epoch = 7;
    const auto rows_of = [&](Document& d, int width) {
        d.begin_frame(width, th.epoch);
        d.materialize_range(0, d.total_rows(), th);
        std::vector<std::string> out;
        for (size_t r = 0; r < d.total_rows(); ++r) {
            std::string t;
            for (const Span& sp : d.line_at(r)->spans) t += sp.text;
            out.push_back(t);
        }
        return out;
    };
    const auto starts = [](const std::string& s, std::string_view p) {
        return s.compare(0, p.size(), p) == 0;
    };

    {
        // 中英混排：在宽字符前后断开，每行（除末行）填满到行宽附近；
        // 闭合标点不到行首，开启标点不留行尾。
        Document d;
        d.append_block(BlockKind::text,
                       "欢迎使用 DAgent 演示终端。这里的 Agent 是脚本模拟的，但界面"
                       "（流式渲染、滚动、选择复制）都是真实的框架行为。");
        const auto rows = rows_of(d, 20);
        BOOST_REQUIRE(rows.size() >= 4u);
        for (size_t i = 0; i < rows.size(); ++i) {
            BOOST_TEST_CONTEXT("row " << i << " '" << rows[i] << "'") {
                for (std::string_view p : {"，", "。", "、", "）"}) {
                    BOOST_TEST(!starts(rows[i], p));
                }
                BOOST_TEST(!rows[i].ends_with("（"));
                if (i + 1 < rows.size()) {
                    Surface probe(40, 1);
                    BOOST_TEST(probe.text(0, 0, rows[i], Style{}) >= 17);
                }
            }
        }
        BOOST_TEST(rows[0] == "欢迎使用 DAgent 演示");
    }
    {
        // 表格单元格隐藏行内标记：列宽按显示文本算，代码片段带 code 样式。
        Document d;
        d.append_block(BlockKind::table,
                       "| 名称 | 说明 |\n| --- | --- |\n| `run()` | **粗体** 与 [链接](x) |\n");
        const auto rows = rows_of(d, 40);
        BOOST_REQUIRE(rows.size() == 3u);
        BOOST_TEST(rows[2] == "│ run() │ 粗体 与 链接 │");
        const Line& ln = *d.line_at(2);
        bool code = false;
        for (const Span& sp : ln.spans) {
            if (sp.text == "run()") code = sp.style == th.markdown_code;
        }
        BOOST_TEST(code);
    }
    {
        // 代码块：每行左侧竖条 + 底色铺满整行，按「宽度 − 2」折行。
        Document d;
        Block code;
        code.kind = BlockKind::code;
        code.meta = "cpp";
        code.source = "int x = 1;\nreturn x;";
        d.append_block(std::move(code));
        const auto rows = rows_of(d, 16);
        BOOST_REQUIRE(rows.size() == 2u);
        BOOST_TEST(starts(rows[0], "▎ int x = 1;"));
        const Line& ln = *d.line_at(0);
        BOOST_TEST(ln.width == 16);
        BOOST_TEST(ln.spans.back().style.bg == th.background_element.bg);
    }
}

BOOST_AUTO_TEST_SUITE_END()
