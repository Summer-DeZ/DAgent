// M5 §3.6/§3.7 验收：文档变更 API（replace / set_meta / erase_from、多个
// open 块、锚点规则）与流式 Markdown 切分（块级结构、任意分块不变性、
// 增量渲染代价）。随机变更序列的每一步都与「相同最终内容新建的
// Document」逐行（文本 + spans 样式）比对，宽度不变的帧走增量计数路径。
// 文档层是纯内存结构，直接断言，不做模拟。

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "tui/document.hpp"
#include "tui/surface.hpp"

// Boost.Test 断言失败时要打印样式，测试 TU 内补上流输出。
namespace dagent::tui {
inline std::ostream& operator<<(std::ostream& os, const Style& st) {
    return os << "Style(attrs=" << static_cast<int>(st.attrs) << ")";
}
} // namespace dagent::tui

using namespace dagent::tui;

namespace {

// 按宽度走一遍帧路径（计数 + 全量物化），取出每行的文本与 spans 样式。
struct Snapshot {
    std::vector<std::string> text;
    std::vector<std::vector<Style>> styles;
};

Snapshot snapshot(Document& d, int width, const Theme& theme = {}) {
    d.begin_frame(width, theme.epoch);
    d.materialize_range(0, d.total_rows(), theme);
    Snapshot out;
    out.text.reserve(d.total_rows());
    out.styles.reserve(d.total_rows());
    for (size_t r = 0; r < d.total_rows(); ++r) {
        const Line* ln = d.line_at(r);
        BOOST_REQUIRE(ln != nullptr);
        std::string s;
        std::vector<Style> styles;
        for (const Span& sp : ln->spans) {
            s += sp.text;
            styles.push_back(sp.style);
        }
        out.text.push_back(std::move(s));
        out.styles.push_back(std::move(styles));
    }
    return out;
}

// 元数据渲染器：一行，内容 = "m:" + meta。meta 变化必须重画它。
class MetaRenderer final : public BlockRenderer {
public:
    WrapResult measure(std::string_view, size_t, int) const override {
        return {1, 1, 0};
    }
    size_t render(const Block& b, int, const Theme&, size_t, size_t,
                  std::vector<Line>& out) const override {
        out.clear();
        out.push_back(Line{{Span{"m:" + b.meta, Style{}}}, 0, 0});
        return 1;
    }
};

// §3.7：块序列（kind/source/meta）+ 逐行渲染结果。
struct StreamSnapshot {
    std::vector<int> kinds; // BlockKind 的整数形式，便于 Boost.Test 打印
    std::vector<std::string> sources;
    std::vector<std::string> metas;
    Snapshot rows;
};

// 各槽位可区分的主题（默认 Theme 的槽位全相同，比对不出样式错误）。
// epoch = 1：与以默认主题物化过的缓存区分开。
Theme distinct_theme() {
    Theme th;
    th.dim.attrs = Attr::dim;
    th.code.attrs = Attr::reverse;
    th.add.attrs = Attr::blink;
    th.del.attrs = Attr::strike;
    th.epoch = 1;
    return th;
}

StreamSnapshot stream_snapshot(Document& d, int width) {
    StreamSnapshot out;
    out.rows = snapshot(d, width, distinct_theme());
    for (size_t i = 0; i < d.block_count(); ++i) {
        const Block& b = d.block_at(i);
        out.kinds.push_back(static_cast<int>(b.kind));
        out.sources.push_back(b.source);
        out.metas.push_back(b.meta);
    }
    return out;
}

// §3.7 验收用的计数型渲染器：累计每帧从增量起点之后处理的字节数。
// 增量正确时，长代码块每帧只重扫最后一行，长段落每帧只重排当前段落。
struct RenderCost {
    size_t frame = 0;
    size_t max = 0;
    void begin_frame() { frame = 0; }
    void add(size_t bytes) {
        frame += bytes;
        if (frame > max) max = frame;
    }
};

class CountingRenderer final : public BlockRenderer {
public:
    CountingRenderer(std::unique_ptr<BlockRenderer> inner, RenderCost& cost)
        : inner_(std::move(inner)), cost_(&cost) {}

    WrapResult measure(std::string_view source, size_t from,
                       int width) const override {
        return inner_->measure(source, from, width);
    }

    size_t render(const Block& block, int width, const Theme& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override {
        cost_->add(block.source.size() -
                   std::min(from, block.source.size()));
        return inner_->render(block, width, theme, from, valid, out);
    }

private:
    std::unique_ptr<BlockRenderer> inner_;
    RenderCost* cost_;
};

std::string random_source(std::mt19937& rng) {
    static const std::vector<std::string> atoms = {
        "a",  "word", " ",   "  ",   "\n",  "中文", "e\U00000301",
        "+",  "-",    "one", "two",  "zzz", "lo",  "ng"};
    std::uniform_int_distribution<std::size_t> pick(0, atoms.size() - 1);
    std::uniform_int_distribution<int> count(0, 8);
    std::string s;
    for (int i = 0, n = count(rng); i < n; ++i) s += atoms[pick(rng)];
    return s;
}

} // namespace

BOOST_AUTO_TEST_SUITE(document)

BOOST_AUTO_TEST_CASE(replace_resets_block_cache_for_any_block) {
    Document d;
    const uint64_t a = d.append_block(BlockKind::text, "one\ntwo");
    const uint64_t b = d.append_block(BlockKind::text, "middle");
    d.append_block(BlockKind::text, "tail");

    BOOST_TEST(snapshot(d, 20).text ==
               (std::vector<std::string>{"one", "two", "middle", "tail"}));

    // 中部块整体替换：旧行缓存必须失效，行数与内容重算。
    BOOST_TEST(d.replace(b, "MID\nDLE\nX"));
    BOOST_TEST(snapshot(d, 20).text ==
               (std::vector<std::string>{"one", "two", "MID", "DLE", "X", "tail"}));

    // 缩到空、再追加到 open 块。
    BOOST_TEST(d.replace(b, ""));
    BOOST_TEST(snapshot(d, 20).text ==
               (std::vector<std::string>{"one", "two", "tail"}));

    Document s;
    const uint64_t open = s.open_block(BlockKind::output, "ab");
    BOOST_TEST(snapshot(s, 20).text == (std::vector<std::string>{"ab"}));
    BOOST_TEST(s.replace(open, "xy"));
    BOOST_TEST(s.append(open, "z"));
    BOOST_TEST(snapshot(s, 20).text == (std::vector<std::string>{"xyz"}));

    BOOST_TEST(!d.replace(9999, "x")); // 不存在
    BOOST_TEST(d.replace(a, "x"));     // 空串/短串替换同样生效
}

BOOST_AUTO_TEST_CASE(set_meta_invalidates_only_materialization) {
    Document d;
    d.set_renderer(BlockKind::code, std::make_unique<MetaRenderer>());
    const uint64_t id = d.append_block(BlockKind::code, "src");

    BOOST_TEST(snapshot(d, 20).text == (std::vector<std::string>{"m:"}));
    BOOST_TEST(d.set_meta(id, "hello"));
    // 物化缓存失效：下一帧用新 meta 重画（计数没变，仍是 1 行）。
    BOOST_TEST(snapshot(d, 20).text == (std::vector<std::string>{"m:hello"}));
    BOOST_TEST(!d.set_meta(9999, "x"));
}

BOOST_AUTO_TEST_CASE(erase_from_drops_suffix_blocks) {
    Document d;
    const uint64_t a = d.append_block(BlockKind::text, "one");
    d.append_block(BlockKind::text, "two");
    const uint64_t c = d.append_block(BlockKind::text, "three");
    d.append_block(BlockKind::text, "four");
    d.begin_frame(20, 0);

    BOOST_TEST(d.erase_from(c) == 2u);
    BOOST_TEST(d.block_count() == 2u);
    BOOST_TEST(d.find(c) == nullptr);
    BOOST_TEST(snapshot(d, 20).text == (std::vector<std::string>{"one", "two"}));

    BOOST_TEST(d.erase_from(9999) == 0u); // 不存在：空操作
    BOOST_TEST(d.erase_from(a) == 2u);
    BOOST_TEST(d.block_count() == 0u);
    BOOST_TEST(snapshot(d, 20).text.empty());
}

BOOST_AUTO_TEST_CASE(replace_clamps_anchor_and_keeps_same_content) {
    const Theme th;
    Document d;
    const uint64_t a = d.append_block(BlockKind::text, "aaa");
    const uint64_t b = d.append_block(BlockKind::text, "hello world foo");
    d.append_block(BlockKind::text, "ccc");
    d.begin_frame(5, th.epoch);
    BOOST_TEST(d.total_rows() == 6u); // 1 + 4 + 1（宽 5：hello / 空格 / world / 空格foo）

    // 锚点字节 6 落在块 b 的 "world"。
    d.set_anchor(Anchor{b, 6, false});
    const size_t row_before = d.row_of(b, 6, th).value();

    // 变长替换：偏移不变，锚点仍指向同一段内容。
    BOOST_TEST(d.replace(b, "hello world foo bar"));
    d.begin_frame(5, th.epoch);
    BOOST_TEST(d.anchor().byte_in_block == 6u);
    BOOST_TEST(d.row_of(b, 6, th).value() == row_before);

    // 变短替换：字节偏移夹到新长度，不越界。
    BOOST_TEST(d.replace(b, "hi"));
    d.begin_frame(5, th.epoch);
    BOOST_TEST(d.anchor().byte_in_block == 2u);
    BOOST_TEST(d.row_of(b, d.anchor().byte_in_block, th).value() < d.total_rows());
    BOOST_TEST(d.find(a) != nullptr);
}

BOOST_AUTO_TEST_CASE(erase_from_repositions_anchor_to_previous_block_end) {
    const Theme th;
    Document d;
    const uint64_t a = d.append_block(BlockKind::text, "aaa\nbb");
    const uint64_t b = d.append_block(BlockKind::text, "middle");
    const uint64_t c = d.append_block(BlockKind::text, "tail");
    d.begin_frame(20, th.epoch);

    // 锚点在 b：删除 c 不影响它。
    d.set_anchor(Anchor{b, 3, false});
    BOOST_TEST(d.erase_from(c) == 1u);
    BOOST_TEST(d.anchor().block_id == b);
    BOOST_TEST(d.anchor().byte_in_block == 3u);

    // 锚点块被删除：落到删除点前最后一块（a）的末尾。
    d.set_anchor(Anchor{b, 2, false});
    BOOST_TEST(d.erase_from(b) == 1u);
    d.begin_frame(20, th.epoch);
    BOOST_TEST(!d.anchor().pinned_to_bottom);
    BOOST_TEST(d.anchor().block_id == a);
    BOOST_TEST(d.anchor().byte_in_block == d.find(a)->source.size());
    BOOST_TEST(d.row_of(a, d.anchor().byte_in_block, th).value() < d.total_rows());

    // 文档已空：贴底。
    BOOST_TEST(d.erase_from(a) == 1u);
    BOOST_TEST(d.anchor().pinned_to_bottom);
    BOOST_TEST(d.total_rows() == 0u);
}

BOOST_AUTO_TEST_CASE(random_mutation_sequence_matches_rebuilt_document) {
    struct Entry {
        BlockKind kind = BlockKind::text;
        std::string source;
        std::string meta;
        bool open = false;
    };

    Document d;
    std::vector<Entry> model;
    std::vector<uint64_t> ids;
    std::mt19937 rng(0x30617);
    std::uniform_int_distribution<std::size_t> pick(0, 1u << 30);
    auto pick_index = [&] {
        return model.empty() ? size_t{0} : pick(rng) % model.size();
    };

    const int widths[] = {13, 5, 40};
    int width = widths[0];
    for (int step = 0; step < 600; ++step) {
        switch (rng() % 8) {
        case 0: { // append 封闭块
            const BlockKind kind = rng() % 2 ? BlockKind::text : BlockKind::output;
            std::string src = random_source(rng);
            const uint64_t id = d.append_block(kind, src);
            model.push_back({kind, std::move(src), "", false});
            ids.push_back(id);
            break;
        }
        case 1: { // open 块（可以有多个）
            const BlockKind kind = rng() % 2 ? BlockKind::diff : BlockKind::code;
            std::string src = random_source(rng);
            const uint64_t id = d.open_block(kind, src);
            model.push_back({kind, std::move(src), "", true});
            ids.push_back(id);
            break;
        }
        case 2: { // 追加到任意 open 块
            if (model.empty()) break;
            const std::size_t i = pick_index();
            if (!model[i].open) break;
            const std::string chunk = random_source(rng);
            if (chunk.empty()) break;
            BOOST_TEST(d.append(ids[i], chunk));
            model[i].source += chunk;
            break;
        }
        case 3: { // 任意块整体替换
            if (model.empty()) break;
            const std::size_t i = pick_index();
            std::string src = random_source(rng);
            BOOST_TEST(d.replace(ids[i], src));
            model[i].source = std::move(src);
            break;
        }
        case 4: { // meta
            if (model.empty()) break;
            const std::size_t i = pick_index();
            BOOST_TEST(d.set_meta(ids[i], "meta" + std::to_string(step)));
            model[i].meta = "meta" + std::to_string(step);
            break;
        }
        case 5: { // 删除后缀
            if (model.empty()) break;
            const std::size_t i = pick_index();
            BOOST_TEST(d.erase_from(ids[i]) == model.size() - i);
            model.resize(i);
            ids.resize(i);
            break;
        }
        case 6: { // close
            if (model.empty()) break;
            const std::size_t i = pick_index();
            BOOST_TEST(d.close_block(ids[i]));
            model[i].open = false;
            break;
        }
        case 7: // 不存在的 id：空操作
            BOOST_TEST(d.erase_from(0xFFFFFFFFu) == 0u);
            BOOST_TEST(!d.replace(0xFFFFFFFFu, "x"));
            BOOST_TEST(!d.set_meta(0xFFFFFFFFu, "x"));
            break;
        default:
            break;
        }

        // 每一步都按当前宽度出帧：宽度不变时 begin_frame 走增量计数
        // （多个 open 块、中部脏块），这才是 §3.6 要验证的路径；宽度
        // 只偶尔变化（整块重数的纪元路径）。
        if (step % 100 == 99) width = widths[(step / 100 + 1) % 3];
        Document rebuilt;
        for (const Entry& e : model) {
            const uint64_t id = rebuilt.append_block(e.kind, e.source);
            rebuilt.set_meta(id, e.meta);
        }
        const Snapshot expect = snapshot(rebuilt, width);
        const Snapshot got = snapshot(d, width);
        const bool same = got.text == expect.text && got.styles == expect.styles;
        size_t first_bad = 0;
        if (!same) {
            const std::size_t n = std::min(got.text.size(), expect.text.size());
            while (first_bad < n && got.text[first_bad] == expect.text[first_bad] &&
                   got.styles[first_bad] == expect.styles[first_bad]) {
                ++first_bad;
            }
        }
        BOOST_TEST_CONTEXT("step " << step << " width " << width
                                   << " first row " << first_bad) {
            BOOST_TEST(same);
        }
        if (!same) break;
    }
    for (std::size_t i = 0; i < model.size(); ++i) {
        BOOST_REQUIRE(d.find(ids[i]) != nullptr);
        BOOST_TEST(d.find(ids[i])->meta == model[i].meta);
    }
}

// ---- §3.7 流式 Markdown 与块渲染器 ----

namespace {

// 覆盖验收要求的全部块级结构：段落、标题、列表、嵌套引用、多语言代码
// 块、表格、分隔线。
std::string markdown_corpus() {
    return
        "# 标题一\n"
        "\n"
        "段落一，包含 **粗体**、*斜体*、`行内代码` 与 [链接](https://example.com/a?b=1)。\n"
        "同一段的第二行，带 __下划线粗体__ 与 _斜体_。\n"
        "\n"
        "## 二级标题\n"
        "标题后的正文。\n"
        "\n"
        "- 列表项一\n"
        "- 列表项二\n"
        "\n"
        "> 第一层引用\n"
        "> > 第二层引用\n"
        "\n"
        "```cpp\n"
        "#include <cstdio>\n"
        "// 注释\n"
        "int main() { return 0; }\n"
        "```\n"
        "\n"
        "```python\n"
        "def add(a, b):\n"
        "    \"\"\"文档字符串\"\"\"\n"
        "    return a + b\n"
        "```\n"
        "\n"
        "```rust\n"
        "fn main() {\n"
        "    let s = r#\"raw\"#;\n"
        "}\n"
        "```\n"
        "\n"
        "| 名称 | 值 | 说明 |\n"
        "| --- | --- | --- |\n"
        "| alpha | 1 | 第一行 |\n"
        "| beta | 22 | 第二行 |\n"
        "\n"
        "---\n"
        "\n"
        "尾段 with `code` 与 [link](x)。\n";
}

} // namespace

BOOST_AUTO_TEST_CASE(markdown_stream_splits_block_level_structure) {
    Document d;
    MarkdownStream s(d);
    s.feed("# T\n");
    s.feed("\n");
    s.feed("para one\n");
    s.feed("\n");
    s.feed("- item\n");
    s.feed("\n");
    s.feed("> quote\n");
    s.feed("\n");
    s.feed("```cpp\n");
    s.feed("int x;\n");
    s.feed("```\n");
    s.feed("\n");
    s.feed("| a | b |\n");
    s.feed("| --- | --- |\n");
    s.feed("| 1 | 2 |\n");
    s.feed("\n");
    s.feed("---\n");
    s.finish();

    struct Expect {
        BlockKind kind;
        const char* source;
        const char* meta;
    };
    const Expect want[] = {
        {BlockKind::markdown, "# T\n", ""},
        {BlockKind::markdown, "para one\n", ""},
        {BlockKind::markdown, "- item\n", ""},
        {BlockKind::markdown, "> quote\n", ""},
        {BlockKind::code, "int x;\n", "cpp"}, // 围栏行不属于 source，info 进 meta
        {BlockKind::table, "| a | b |\n| --- | --- |\n| 1 | 2 |\n", ""},
        {BlockKind::markdown, "---\n", ""},
    };
    BOOST_REQUIRE(d.block_count() == std::size(want));
    for (size_t i = 0; i < std::size(want); ++i) {
        BOOST_TEST_CONTEXT("block " << i) {
            BOOST_TEST(static_cast<int>(d.block_at(i).kind) ==
                       static_cast<int>(want[i].kind));
            BOOST_TEST(d.block_at(i).source == want[i].source);
            BOOST_TEST(d.block_at(i).meta == want[i].meta);
            BOOST_TEST(!d.block_at(i).open);
        }
    }
}

BOOST_AUTO_TEST_CASE(markdown_stream_table_interrupts_paragraph) {
    {
        // 表格可以打断段落（无空行）；结束后下一行重新开 markdown 块。
        Document d;
        MarkdownStream s(d);
        s.feed("intro\n| a | b |\n|---|---|\n| 1 | 2 |\nend\n");
        s.finish();
        BOOST_REQUIRE(d.block_count() == 3);
        BOOST_TEST(d.block_at(0).source == std::string("intro\n"));
        BOOST_TEST(static_cast<int>(d.block_at(1).kind) == static_cast<int>(BlockKind::table));
        BOOST_TEST(d.block_at(1).source ==
                   std::string("| a | b |\n|---|---|\n| 1 | 2 |\n"));
        BOOST_TEST(d.block_at(2).source == std::string("end\n"));
    }
    {
        // '|' 开头但下一行不是分隔行：按普通段落留在同一块。
        Document d;
        MarkdownStream s(d);
        s.feed("| not | table\nplain\n");
        s.finish();
        BOOST_REQUIRE(d.block_count() == 1);
        BOOST_TEST(static_cast<int>(d.block_at(0).kind) == static_cast<int>(BlockKind::markdown));
        BOOST_TEST(d.block_at(0).source == std::string("| not | table\nplain\n"));
    }
    {
        // 表格与围栏之间不需要空行：非 '|' 行结束表格并开启新结构。
        Document d;
        MarkdownStream s(d);
        s.feed("| a |\n|---|\n```py\nx = 1\n```\n");
        s.finish();
        BOOST_REQUIRE(d.block_count() == 2);
        BOOST_TEST(static_cast<int>(d.block_at(0).kind) == static_cast<int>(BlockKind::table));
        BOOST_TEST(d.block_at(0).source == std::string("| a |\n|---|\n"));
        BOOST_TEST(static_cast<int>(d.block_at(1).kind) == static_cast<int>(BlockKind::code));
        BOOST_TEST(d.block_at(1).source == std::string("x = 1\n"));
        BOOST_TEST(d.block_at(1).meta == std::string("py"));
    }
    {
        // 围栏闭合后紧跟表格：表格需要看下一行，这里由 feed 的第二行确认。
        Document d;
        MarkdownStream s(d);
        s.feed("```go\ny := 1\n```\n| a |\n|---|\n");
        s.finish();
        BOOST_REQUIRE(d.block_count() == 2);
        BOOST_TEST(static_cast<int>(d.block_at(0).kind) == static_cast<int>(BlockKind::code));
        BOOST_TEST(static_cast<int>(d.block_at(1).kind) == static_cast<int>(BlockKind::table));
    }
}

BOOST_AUTO_TEST_CASE(markdown_stream_chunking_matches_whole_feed) {
    const std::string corpus = markdown_corpus();

    Document whole;
    MarkdownStream whole_stream(whole);
    whole_stream.feed(corpus);
    whole_stream.finish();

    std::mt19937 rng(0x30707);
    const int widths[] = {72, 24, 9};

    // 随机大小分块：块序列与逐行渲染（含 spans 样式）都必须与整份喂入相同。
    for (int width : widths) {
        const StreamSnapshot expect = stream_snapshot(whole, width);
        for (int iter = 0; iter < 25; ++iter) {
            Document d;
            MarkdownStream s(d);
            std::uniform_int_distribution<size_t> pick(1, 11);
            for (size_t i = 0; i < corpus.size();) {
                const size_t n = pick(rng);
                s.feed(std::string_view(corpus).substr(i, n));
                i += n;
            }
            s.finish();
            const StreamSnapshot got = stream_snapshot(d, width);
            BOOST_TEST_CONTEXT("width " << width << " iter " << iter) {
                BOOST_TEST(got.kinds == expect.kinds);
                BOOST_TEST(got.sources == expect.sources);
                BOOST_TEST(got.metas == expect.metas);
                BOOST_TEST(got.rows.text == expect.rows.text);
                BOOST_TEST(got.rows.styles == expect.rows.styles);
            }
            if (got.sources != expect.sources) break;
        }
    }

    // 小块喂入且每块出帧、宽度中途变化：覆盖增量计数/物化与跨行词法
    // 状态的续扫路径，最终结果仍与整份喂入一致。
    Document d;
    MarkdownStream s(d);
    int frame_width = 24;
    for (size_t i = 0; i < corpus.size();) {
        const size_t n = 1 + rng() % 5;
        s.feed(std::string_view(corpus).substr(i, n));
        i += n;
        d.begin_frame(frame_width, 0);
        d.materialize_range(0, d.total_rows(), {});
        if (rng() % 4 == 0) frame_width = frame_width == 24 ? 17 : 24;
    }
    s.finish();
    d.begin_frame(frame_width, 0);
    d.materialize_range(0, d.total_rows(), {});

    Document rebuilt;
    MarkdownStream rebuilt_stream(rebuilt);
    rebuilt_stream.feed(corpus);
    rebuilt_stream.finish();
    const StreamSnapshot expect = stream_snapshot(rebuilt, frame_width);
    const StreamSnapshot got = stream_snapshot(d, frame_width);
    BOOST_TEST_CONTEXT("framed width " << frame_width) {
        BOOST_TEST(got.kinds == expect.kinds);
        BOOST_TEST(got.sources == expect.sources);
        BOOST_TEST(got.metas == expect.metas);
        BOOST_TEST(got.rows.text == expect.rows.text);
        BOOST_TEST(got.rows.styles == expect.rows.styles);
    }
}

BOOST_AUTO_TEST_CASE(streaming_code_block_render_cost_is_bounded_by_last_line) {
    RenderCost cost;
    Document d;
    d.set_renderer(BlockKind::code, std::make_unique<CountingRenderer>(
                                         std::make_unique<SyntaxRenderer>(), cost));
    MarkdownStream s(d);
    s.feed("```cpp\n");

    std::string line;
    for (int i = 0; i < 400; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "int value_%04d = %d;\n", i, i);
        line = buf;
        s.feed(line);
        cost.begin_frame();
        d.begin_frame(80, 0);
        d.materialize_range(0, d.total_rows(), {});
        BOOST_TEST_CONTEXT("line " << i) {
            BOOST_TEST(cost.frame <= line.size() + 16);
        }
    }
    s.feed("```\n");
    s.finish();
    BOOST_TEST(cost.max <= line.size() + 16);
}

BOOST_AUTO_TEST_CASE(streaming_paragraph_render_cost_is_bounded_by_paragraph) {
    RenderCost cost;
    Document d;
    d.set_renderer(
        BlockKind::markdown,
        std::make_unique<CountingRenderer>(std::make_unique<MarkdownRenderer>(),
                                           cost));
    MarkdownStream s(d);

    // 先有若干已关闭的段落：只有当前段落重排时，每帧代价才受它约束；
    // 若整条消息重排，代价会包含前面这些段落。
    for (int p = 0; p < 5; ++p) {
        std::string done;
        for (int i = 0; i < 60; ++i) done += "earlier" + std::to_string(i) + " ";
        s.feed(done + "\n\n");
    }
    d.begin_frame(80, 0);
    d.materialize_range(0, d.total_rows(), {});
    cost = RenderCost{}; // 前面段落的首次物化不计入流式代价

    std::string paragraph;
    for (int i = 0; i < 300; ++i) {
        std::string word = "word" + std::to_string(i) + " ";
        paragraph += word;
        s.feed(word);
        cost.begin_frame();
        d.begin_frame(80, 0);
        d.materialize_range(0, d.total_rows(), {});
        BOOST_TEST_CONTEXT("chunk " << i) {
            BOOST_TEST(cost.frame <= paragraph.size());
        }
    }
    s.finish();
    BOOST_TEST(cost.max <= paragraph.size());
}

BOOST_AUTO_TEST_CASE(markdown_markers_are_hidden_and_styles_survive_wrapping) {
    const Theme th = distinct_theme();
    auto rows_of = [&](Document& d, int width) {
        d.begin_frame(width, th.epoch);
        d.materialize_range(0, d.total_rows(), th);
        std::vector<const Line*> rows;
        for (size_t r = 0; r < d.total_rows(); ++r) rows.push_back(d.line_at(r));
        return rows;
    };
    auto text_of = [](const Line& ln) {
        std::string t;
        for (const Span& sp : ln.spans) t += sp.text;
        return t;
    };

    {
        // 粗体跨折行：标记隐藏，两行的正文都保持粗体；按显示文本折行。
        Document d;
        MarkdownStream s(d);
        s.feed("aaaa bbbb **bold text here** tail\n");
        s.finish();
        const auto rows = rows_of(d, 16);
        BOOST_REQUIRE(rows.size() == 2u);
        BOOST_TEST(text_of(*rows[0]) == "aaaa bbbb bold ");
        BOOST_TEST(text_of(*rows[1]) == "text here tail");
        BOOST_TEST(rows[0]->width == 15);
        BOOST_TEST(rows[1]->spans[0].text == "text here");
        BOOST_TEST(any(rows[1]->spans[0].style.attrs & Attr::bold));
        BOOST_TEST(rows[1]->offset == 17u); // 续行的锚点偏移指向原文的 "text"
    }
    {
        // 链接只显示文本，行内代码内的星号按字面；孤立的 * 不当强调。
        Document d;
        MarkdownStream s(d);
        s.feed("see [docs](https://example.com/a) and `a*b*c`, 2 * 3 * 4\n");
        s.finish();
        const auto rows = rows_of(d, 80);
        BOOST_REQUIRE(rows.size() == 1u);
        BOOST_TEST(text_of(*rows[0]) == "see docs and a*b*c, 2 * 3 * 4");
        BOOST_TEST(rows[0]->spans[1].text == "docs");
        BOOST_TEST(any(rows[0]->spans[1].style.attrs & Attr::underline));
        BOOST_TEST(rows[0]->spans[3].text == "a*b*c");
        BOOST_TEST(rows[0]->spans[3].style == th.code);
    }
    {
        // 列表项的续行留在同一块并对齐到正文；强调可跨软换行。
        Document d;
        MarkdownStream s(d);
        s.feed("- item *one\n  continued* text\n- item two\n");
        s.finish();
        BOOST_REQUIRE(d.block_count() == 1u);
        const auto rows = rows_of(d, 40);
        BOOST_REQUIRE(rows.size() == 3u);
        BOOST_TEST(text_of(*rows[0]) == "• item one");
        BOOST_TEST(text_of(*rows[1]) == "  continued text");
        BOOST_TEST(text_of(*rows[2]) == "• item two");
        BOOST_TEST(any(rows[1]->spans[1].style.attrs & Attr::italic));
        BOOST_TEST(rows[1]->spans[1].text == "continued");
    }
}

BOOST_AUTO_TEST_CASE(code_highlighting_is_not_cut_by_wrapping) {
    // 被折行的行注释：每一行都保持注释样式（折行后的 while 不当关键字）；
    // 流式逐字喂入（每帧增量续扫）与整份喂入的结果相同。
    const Theme th = distinct_theme();
    const std::string code =
        "```cpp\n"
        "int f(); // if return while comment keeps going on\n"
        "auto s = \"a long string with if and while\"; call(s);\n"
        "```\n";

    Document whole;
    MarkdownStream ws(whole);
    ws.feed(code);
    ws.finish();
    whole.begin_frame(16, th.epoch);
    whole.materialize_range(0, whole.total_rows(), th);

    // 第一条逻辑行："//" 之后的所有 span（含折行后的续行）都是注释样式。
    // 第二条逻辑行：被折行的字符串里的 if/while 不当关键字，加粗的只有 auto。
    const std::string& src = whole.block_at(0).source;
    const size_t line1_end = src.find('\n');
    bool seen = false;
    size_t comment_rows = 0;
    size_t string_rows = 0;
    for (size_t r = 0; r < whole.total_rows(); ++r) {
        const Line& ln = *whole.line_at(r);
        const bool line1 = ln.offset < line1_end;
        for (const Span& sp : ln.spans) {
            BOOST_TEST_CONTEXT("row " << r << " span '" << sp.text << "'") {
                if (line1) {
                    if (sp.text.find("//") != std::string::npos) seen = true;
                    if (seen) BOOST_TEST(sp.style == th.dim);
                } else if (any(sp.style.attrs & Attr::bold)) {
                    BOOST_TEST(sp.text == "auto");
                }
            }
            if (!line1 && sp.style == th.code) ++string_rows;
        }
        if (line1 && seen) ++comment_rows;
    }
    BOOST_TEST(string_rows >= 2u); // 字符串确实跨了折行
    BOOST_TEST(comment_rows >= 3u); // 注释确实被折成了多行

    Document streamed;
    MarkdownStream ss(streamed);
    for (const char c : code) {
        ss.feed(std::string_view(&c, 1));
        streamed.begin_frame(16, th.epoch);
        streamed.materialize_range(0, streamed.total_rows(), th);
    }
    ss.finish();
    const Snapshot a = snapshot(whole, 16, th);
    const Snapshot b = snapshot(streamed, 16, th);
    BOOST_TEST(a.text == b.text);
    BOOST_TEST(a.styles == b.styles);
}

BOOST_AUTO_TEST_CASE(block_margins_space_paragraphs_and_never_trap_scrolling) {
    {
        // 消息内块之间空 1 行；源码里的空行不进块；消息第一块用 first_margin。
        Document d;
        MarkdownStream s(d);
        s.feed("# T\n\npara\n- a\n");
        s.finish();
        MarkdownStream next(d, 2);
        next.feed("second\n");
        next.finish();
        BOOST_TEST(snapshot(d, 20).text ==
                   (std::vector<std::string>{"# T", "", "para", "", "• a", "", "",
                                             "second"}));
    }
    {
        // 流式新开的空块边距折叠：代码块有内容之前不冒出空行。
        Document d;
        MarkdownStream s(d);
        s.feed("para\n\n```py\n");
        BOOST_TEST(snapshot(d, 20).text == (std::vector<std::string>{"para"}));
        s.feed("x = 1\n");
        BOOST_TEST(snapshot(d, 20).text ==
                   (std::vector<std::string>{"para", "", "x = 1"}));
    }
    {
        // 逐行滚动：视口顶行从不停在边距行上，上下都不会卡住。
        Scrollback sb;
        MarkdownStream s(sb.document());
        for (int i = 0; i < 10; ++i) s.feed("p" + std::to_string(i) + "\n\n");
        s.finish();
        sb.layout({0, 0, 20, 2});
        Surface surf(20, 2);
        auto top_text = [&] {
            sb.render(surf);
            return std::string(surf.at(0, 0).grapheme()) +
                   std::string(surf.at(1, 0).grapheme());
        };
        top_text();
        sb.scroll_home();
        BOOST_TEST(top_text() == "p0");
        for (int i = 1; i <= 8; ++i) {
            sb.scroll_lines(1);
            BOOST_TEST(top_text() == "p" + std::to_string(i));
        }
        sb.scroll_lines(1);
        top_text();
        BOOST_TEST(sb.pinned()); // 到底：贴底
        for (int i = 8; i >= 0; --i) {
            sb.scroll_lines(-1);
            BOOST_TEST(top_text() == "p" + std::to_string(i));
        }
        sb.scroll_lines(-1);
        BOOST_TEST(top_text() == "p0"); // 顶部夹住
    }
}

BOOST_AUTO_TEST_SUITE_END()
