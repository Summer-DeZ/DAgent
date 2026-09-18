// 文档层（§4.5–§4.6）：流式 Markdown 分块、块渲染器与主题令牌、滚动区锚点、
// 选择与复制。直接驱动真实的 Document / MarkdownStream / Scrollback。

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "tui/document.hpp"
#include "tui/surface.hpp"

// Boost.Test 断言失败时打印位置与颜色。
namespace dagent::tui {
inline std::ostream& operator<<(std::ostream& os, const Location& l) {
    return os << "Location{" << l.block_id << "," << l.byte_in_block << "}";
}
inline std::ostream& operator<<(std::ostream& os, const Color& c) {
    return os << "Color{" << static_cast<int>(c.kind) << "," << static_cast<int>(c.r)
              << "," << static_cast<int>(c.g) << "," << static_cast<int>(c.b) << "}";
}
} // namespace dagent::tui

using namespace dagent::tui;

namespace {

// 按宽度折行并物化全部行，返回每行的显示文本。
std::vector<std::string> rows_of(Document& d, int width, const ThemeTokens& th) {
    d.begin_frame(width, th.epoch);
    d.materialize_range(0, d.total_rows(), th);
    std::vector<std::string> out;
    for (size_t r = 0; r < d.total_rows(); ++r) {
        std::string t;
        for (const Span& sp : d.line_at(r)->spans) t += sp.text;
        out.push_back(t);
    }
    return out;
}

// 某行中显示文本为 text 的片段的样式。
const Style* span_style(const Document& d, size_t row, std::string_view text) {
    for (const Span& sp : d.line_at(row)->spans) {
        if (sp.text == text) return &sp.style;
    }
    return nullptr;
}

void render(Scrollback& sb, Surface& s, int width, int height) {
    s.resize(width, height);
    sb.layout({0, 0, width, height});
    sb.render(s);
}

// 反色单元格按行序拼接（选区高亮的屏幕内容）。
std::string highlighted(const Surface& s) {
    std::string out;
    for (int r = 0; r < s.rows(); ++r) {
        for (int c = 0; c < s.cols(); ++c) {
            const Cell& cell = s.at(c, r);
            if (cell.width != 0 && any(cell.style.attrs & Attr::reverse)) {
                out += cell.grapheme();
            }
        }
    }
    return out;
}

constexpr std::string_view k_message =
    "# 标题\n\n段落第一行，**粗体\n跨行**结束。\n- 列表一\n- 列表 `二`\n\n"
    "```cpp\nint main() { return 0; }\n```\n"
    "| 名称 | 值 |\n| --- | --- |\n| α | 1 |\n\n> 引用\n---\n尾段没有换行";

} // namespace

BOOST_AUTO_TEST_SUITE(document)

// 流式 Markdown 按块级结构拆块，只有最后一块在增长；结果与喂入的分块方式无关。
BOOST_AUTO_TEST_CASE(markdown_stream_splits_blocks_independent_of_chunking) {
    const ThemeTokens& th = dark_theme();
    Document ref;
    {
        MarkdownStream ms(ref, 2);
        ms.feed(k_message);
        ms.finish();
    }
    const BlockKind kinds[] = {BlockKind::markdown, BlockKind::markdown, BlockKind::markdown,
                               BlockKind::code,     BlockKind::table,    BlockKind::markdown,
                               BlockKind::markdown, BlockKind::markdown};
    BOOST_REQUIRE(ref.block_count() == std::size(kinds));
    for (size_t i = 0; i < ref.block_count(); ++i) {
        BOOST_TEST_CONTEXT("block " << i) {
            BOOST_TEST((ref.block_at(i).kind == kinds[i]));
            BOOST_TEST(ref.block_at(i).margin_top == (i == 0 ? 2 : 1)); // 首块取构造参数
            BOOST_TEST(!ref.block_at(i).open);
        }
    }
    // 围栏行不进 source，语言进 meta。
    BOOST_TEST(ref.block_at(3).source == "int main() { return 0; }\n");
    BOOST_TEST(ref.block_at(3).meta == "cpp");
    BOOST_TEST(ref.block_at(1).source == "段落第一行，**粗体\n跨行**结束。\n");
    BOOST_TEST(ref.block_at(7).source == "尾段没有换行");
    const std::vector<std::string> want = rows_of(ref, 24, th);

    for (const size_t step : {1u, 2u, 3u, 5u, 7u, 13u}) {
        BOOST_TEST_CONTEXT("chunk " << step) {
            Document d;
            MarkdownStream ms(d, 2);
            bool only_last_open = true;
            for (size_t at = 0; at < k_message.size(); at += step) {
                ms.feed(k_message.substr(at, step)); // 会切在多字节字符中间
                for (size_t i = 0; i + 1 < d.block_count(); ++i) {
                    if (d.block_at(i).open) only_last_open = false;
                }
            }
            ms.finish();
            BOOST_TEST(only_last_open);
            BOOST_REQUIRE(d.block_count() == ref.block_count());
            for (size_t i = 0; i < d.block_count(); ++i) {
                BOOST_TEST((d.block_at(i).kind == ref.block_at(i).kind));
                BOOST_TEST(d.block_at(i).source == ref.block_at(i).source);
                BOOST_TEST(d.block_at(i).meta == ref.block_at(i).meta);
                BOOST_TEST(d.block_at(i).open == false);
            }
            BOOST_TEST(rows_of(d, 24, th) == want, boost::test_tools::per_element());
        }
    }
}

// 各类块的显示：Markdown 隐藏标记、表格对齐、代码竖条与高亮、diff 分色、中文折行；
// 样式全部来自主题令牌，递增 epoch 后整体刷新。
BOOST_AUTO_TEST_CASE(block_renderers_display_with_theme_tokens) {
    ThemeTokens th = dark_theme();
    th.epoch = 1;
    {
        Document d;
        MarkdownStream ms(d);
        ms.feed(k_message);
        ms.finish();
        const auto rows = rows_of(d, 24, th);
        BOOST_REQUIRE(rows.size() == 20u);
        BOOST_TEST(rows[0] == "# 标题");
        BOOST_TEST(rows[2] == "段落第一行，粗体"); // 强调跨行配对，标记隐藏
        BOOST_TEST(rows[3] == "跨行结束。");
        BOOST_TEST(rows[5] == "• 列表一");
        BOOST_TEST(rows[6] == "• 列表 二");
        BOOST_TEST(rows[8].starts_with("▎ int main() { return"));
        BOOST_TEST(rows[11] == "│ 名称 │ 值 │");
        BOOST_TEST(rows[12] == "├──────┼────┤");
        BOOST_TEST(rows[13] == "│ α    │ 1  │");
        BOOST_TEST(rows[15] == "│ 引用");
        std::string rule;
        for (int i = 0; i < 24; ++i) rule += "─";
        BOOST_TEST(rows[17] == rule); // 分隔线画满整行
        BOOST_TEST((*span_style(d, 0, "标题") == th.markdown_heading));
        BOOST_TEST((*span_style(d, 6, "二") == th.markdown_code));
        BOOST_TEST(any(span_style(d, 2, "粗体")->attrs & Attr::bold));
        BOOST_TEST((span_style(d, 8, "int")->fg == th.syntax_keyword.fg));
        // 代码块整行铺底色（竖条 2 列，按 宽度 − 2 折行）。
        BOOST_TEST(d.line_at(8)->width == 24);
        BOOST_TEST(d.line_at(8)->spans.back().style.bg == th.background_element.bg);
    }
    {
        // 中英混排：汉字前后可断行，每行（除末行）接近填满；闭合标点不到行首。
        Document d;
        d.append_block(BlockKind::text,
                       "欢迎使用 DAgent 演示终端。这里的 Agent 是脚本模拟的，但界面"
                       "（流式渲染、滚动、选择复制）都是真实的框架行为。");
        const auto rows = rows_of(d, 20, th);
        BOOST_REQUIRE(rows.size() >= 4u);
        BOOST_TEST(rows[0] == "欢迎使用 DAgent 演示");
        for (size_t i = 0; i < rows.size(); ++i) {
            BOOST_TEST_CONTEXT("row " << i << " '" << rows[i] << "'") {
                for (std::string_view p : {"，", "。", "、", "）"}) {
                    BOOST_TEST(!rows[i].starts_with(p));
                }
                BOOST_TEST(!rows[i].ends_with("（"));
                if (i + 1 < rows.size()) {
                    Surface probe(40, 1);
                    BOOST_TEST(probe.text(0, 0, rows[i], Style{}) >= 17);
                }
            }
        }
    }

    // 主题令牌：每个渲染器的样式都取自令牌；只改令牌不递增 epoch 不刷新。
    Scrollback sb;
    Document& d = sb.document();
    d.append_block(BlockKind::text, "plain");
    Block code;
    code.kind = BlockKind::code;
    code.source = "int x;";
    code.meta = "cpp";
    d.append_block(std::move(code));
    d.append_block(BlockKind::diff, "+add\n-del");
    d.append_block(BlockKind::markdown, "# h");
    const auto make = [](uint8_t base) {
        ThemeTokens t;
        t.text = {Color::indexed(base), Color{}, Attr::none};
        t.syntax_keyword = {Color::indexed(static_cast<uint8_t>(base + 1)), Color{}, Attr::none};
        t.diff_added = {Color::indexed(static_cast<uint8_t>(base + 2)), Color{}, Attr::none};
        t.diff_removed = {Color::indexed(static_cast<uint8_t>(base + 3)), Color{}, Attr::none};
        t.markdown_heading = {Color::indexed(static_cast<uint8_t>(base + 4)), Color{}, Attr::none};
        return t;
    };
    Surface s;
    const auto fg = [&](int x, int y) { return s.at(x, y).style.fg; };
    ThemeTokens first = make(10);
    first.epoch = 1;
    sb.set_theme(first);
    render(sb, s, 20, 6);
    BOOST_TEST(fg(0, 0) == Color::indexed(10));
    BOOST_TEST(fg(2, 1) == Color::indexed(11));
    BOOST_TEST(fg(0, 2) == Color::indexed(12));
    BOOST_TEST(fg(0, 3) == Color::indexed(13));
    BOOST_TEST(fg(0, 4) == Color::indexed(14));

    ThemeTokens stale = make(20);
    stale.epoch = first.epoch;
    sb.set_theme(stale);
    render(sb, s, 20, 6);
    BOOST_TEST(fg(0, 0) == Color::indexed(10));

    ThemeTokens next = make(20);
    next.epoch = first.epoch + 1;
    sb.set_theme(next);
    render(sb, s, 20, 6);
    BOOST_TEST(fg(0, 0) == Color::indexed(20));
    BOOST_TEST(fg(2, 1) == Color::indexed(21));
    BOOST_TEST(fg(0, 2) == Color::indexed(22));
    BOOST_TEST(fg(0, 3) == Color::indexed(23));
    BOOST_TEST(fg(0, 4) == Color::indexed(24));

    // 按终端背景选明暗：取不到或是索引色时取 dark。
    BOOST_TEST(&default_theme(std::nullopt) == &dark_theme());
    BOOST_TEST(&default_theme(Color::rgb(16, 16, 16)) == &dark_theme());
    BOOST_TEST(&default_theme(Color::rgb(250, 250, 250)) == &light_theme());
    BOOST_TEST(&default_theme(Color::indexed(15)) == &dark_theme());
}

// 滚动区：贴底时跟随新内容；翻上去后，追加、改宽度、裁掉旧块都不让正在看的内容跳走。
BOOST_AUTO_TEST_CASE(scrollback_view_follows_content_not_rows) {
    Scrollback sb;
    Document& d = sb.document();
    for (int i = 0; i < 20; ++i) {
        d.append_block(BlockKind::text,
                       "块" + std::to_string(i) + " alpha beta gamma delta");
    }
    Surface s;
    render(sb, s, 20, 6);
    BOOST_TEST(sb.pinned());
    BOOST_TEST(sb.hit({0, 5})->block_id == d.block_at(19).id);

    // 贴底：新内容到达即显示在底部。
    const uint64_t fresh = d.append_block(BlockKind::text, "新内容");
    render(sb, s, 20, 6);
    BOOST_TEST(sb.hit({0, 5})->block_id == fresh);
    BOOST_TEST(sb.unseen_rows() == 0u);

    // 翻上去：追加内容不动视口，底下未读行数增加。
    sb.scroll_lines(-9);
    render(sb, s, 20, 6);
    BOOST_TEST(!sb.pinned());
    const Location top = *sb.hit({0, 0});
    const size_t unseen = sb.unseen_rows();
    for (int i = 0; i < 3; ++i) d.append_block(BlockKind::text, "more");
    render(sb, s, 20, 6);
    BOOST_TEST(*sb.hit({0, 0}) == top);
    BOOST_TEST(sb.unseen_rows() == unseen + 3);

    // 改宽度：视口顶行仍是包含同一字节的那一行；改回来位置完全复原。
    render(sb, s, 9, 6);
    const Location narrow_top = *sb.hit({0, 0});
    BOOST_TEST(narrow_top.block_id == top.block_id);
    BOOST_TEST(narrow_top <= top);
    BOOST_TEST(top < *sb.hit({0, 1}));
    render(sb, s, 20, 6);
    BOOST_TEST(*sb.hit({0, 0}) == top);

    // 裁掉视口之前的旧块：视口不动。
    size_t index = 0;
    while (d.block_at(index).id != top.block_id) ++index;
    d.trim_blocks(d.block_count() - index);
    render(sb, s, 20, 6);
    BOOST_TEST(d.block_at(0).id == top.block_id);
    BOOST_TEST(*sb.hit({0, 0}) == top);

    // 折叠：块最多显示指定行数。
    d.begin_frame(20, sb.theme().epoch);
    const size_t before = d.total_rows();
    BOOST_TEST(d.set_collapsed(top.block_id, true, 1));
    d.begin_frame(20, sb.theme().epoch);
    BOOST_TEST(d.total_rows() == before - 1); // 两行的块折成一行

    // 回到底部并恢复跟随。
    sb.scroll_end();
    render(sb, s, 20, 6);
    BOOST_TEST(sb.pinned());
    BOOST_TEST(sb.hit({0, 5})->block_id == d.block_at(d.block_count() - 1).id);
}

// 选择：屏幕坐标换算成源位置，复制出的是源文本（含 Markdown 标记、不含折行换行）；
// 改变宽度后选区与高亮仍在同一段内容上。
BOOST_AUTO_TEST_CASE(selection_copies_source_and_survives_width_change) {
    Scrollback sb;
    Document& d = sb.document();
    const uint64_t a = d.append_block(BlockKind::text, "alpha beta gamma delta epsilon");
    Block md;
    md.kind = BlockKind::markdown;
    md.source = "**bold** tail\n";
    md.margin_top = 1;
    const uint64_t b = d.append_block(std::move(md));

    Surface s;
    render(sb, s, 12, 6);
    // 宽 12：A 折成三行，空一行（B 的上边距），B 显示为 "bold tail"。
    BOOST_TEST(s.at(0, 1).grapheme() == "g");
    BOOST_TEST(s.at(0, 4).grapheme() == "b");

    const auto from = sb.hit({0, 0});
    const auto to = sb.hit({3, 4}); // "bold" 的 d，源里在 "**" 之后
    BOOST_REQUIRE(from && to);
    BOOST_TEST(*from == (Location{a, 0}));
    BOOST_TEST(*to == (Location{b, 5}));
    sb.select({*to, *from}); // 反向拖拽等价
    BOOST_TEST(sb.selected_text() == "alpha beta gamma delta epsilon\n\n**bold");

    // 双击选词、三击选逻辑行。
    const Selection word = d.word_around(*sb.hit({2, 1}));
    BOOST_TEST(d.text_between(word.anchor, word.head) == "gamma");
    const Selection line = d.line_around(*sb.hit({6, 4}));
    BOOST_TEST(d.text_between(line.anchor, line.head) == "**bold** tail");

    // 选中 "beta gamma delta"（跨软折行），换几种宽度后选区与高亮不变。
    const Location lo = *sb.hit({6, 0});
    const Location hi = *sb.hit({10, 1});
    sb.select({lo, hi});
    BOOST_TEST(sb.selected_text() == "beta gamma delta");
    for (const int width : {7, 30, 9}) {
        render(sb, s, width, 8);
        BOOST_TEST_CONTEXT("width " << width) {
            BOOST_TEST(sb.selected_text() == "beta gamma delta");
            BOOST_TEST(highlighted(s) == "beta gamma delta");
        }
    }
    sb.clear_selection();
    render(sb, s, 9, 8);
    BOOST_TEST(highlighted(s).empty());
}

BOOST_AUTO_TEST_SUITE_END()
