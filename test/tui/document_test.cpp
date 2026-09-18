// M5 §3.6 文档变更 API 验收：replace / set_meta / erase_from、多个 open
// 块、锚点规则。随机变更序列的每一步都与「相同最终内容新建的 Document」
// 逐行（文本 + spans 样式）比对，宽度不变的帧走增量计数路径。文档层是
// 纯内存结构，直接断言，不做模拟。

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "tui/document.hpp"

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

BOOST_AUTO_TEST_SUITE_END()
