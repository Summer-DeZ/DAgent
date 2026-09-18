// M6 §3.13 验收：生成表对 Unicode 官方 GraphemeBreakTest.txt 的切分结果
// 一致（按清单排除 GB9b/GB9c 用例），emoji/VS16 宽度为 2，intern 表超过
// 上限后清表并整屏重画、内容与全量渲染一致。
//
// 测试数据来自 Unicode 官方（test/tui/data/GraphemeBreakTest.txt，
// 与 grapheme.cpp 生成区段同一版本），不做模拟。

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "tui/grapheme.hpp"
#include "tui/surface.hpp"

using namespace dagent::tui;

namespace {

struct Range {
    char32_t lo;
    char32_t hi;
};

// Unicode 17.0.0 的 GB9b/GB9c 相关码点清单：GraphemeBreakProperty 的
// Prepend，以及 DerivedCoreProperties 的 InCB Consonant / Linker。
// 由 temp/gen_unicode_tables.py 生成（脚本不入库，与 grapheme.cpp 的
// 生成区段同源）。
constexpr Range kExcludedPrepend[] = {
    {0x600, 0x605},   {0x6DD, 0x6DD},   {0x70F, 0x70F},   {0x890, 0x891},
    {0x8E2, 0x8E2},   {0xD4E, 0xD4E},   {0x110BD, 0x110BD}, {0x110CD, 0x110CD},
    {0x111C2, 0x111C3}, {0x113D1, 0x113D1}, {0x1193F, 0x1193F},
    {0x11941, 0x11941}, {0x11A84, 0x11A89}, {0x11D46, 0x11D46},
    {0x11F02, 0x11F02},
};
constexpr Range kExcludedIncbConsonant[] = {
    {0x915, 0x939},  {0x958, 0x95F},  {0x978, 0x97F},  {0x995, 0x9A8},
    {0x9AA, 0x9B0},  {0x9B2, 0x9B2},  {0x9B6, 0x9B9},  {0x9DC, 0x9DD},
    {0x9DF, 0x9DF},  {0x9F0, 0x9F1},  {0xA95, 0xAA8},  {0xAAA, 0xAB0},
    {0xAB2, 0xAB3},  {0xAB5, 0xAB9},  {0xAF9, 0xAF9},  {0xB15, 0xB28},
    {0xB2A, 0xB30},  {0xB32, 0xB33},  {0xB35, 0xB39},  {0xB5C, 0xB5D},
    {0xB5F, 0xB5F},  {0xB71, 0xB71},  {0xC15, 0xC28},  {0xC2A, 0xC39},
    {0xC58, 0xC5A},  {0xD15, 0xD3A},  {0x1000, 0x102A}, {0x103F, 0x103F},
    {0x1050, 0x1055}, {0x105A, 0x105D}, {0x1061, 0x1061}, {0x1065, 0x1066},
    {0x106E, 0x1070}, {0x1075, 0x1081}, {0x108E, 0x108E}, {0x1780, 0x17B3},
    {0x1A20, 0x1A54}, {0x1B0B, 0x1B0C}, {0x1B13, 0x1B33}, {0x1B45, 0x1B4C},
    {0x1B83, 0x1BA0}, {0x1BAE, 0x1BAF}, {0x1BBB, 0x1BBD}, {0xA989, 0xA98B},
    {0xA98F, 0xA9B2}, {0xA9E0, 0xA9E4}, {0xA9E7, 0xA9EF}, {0xA9FA, 0xA9FE},
    {0xAA60, 0xAA6F}, {0xAA71, 0xAA73}, {0xAA7A, 0xAA7A}, {0xAA7E, 0xAA7F},
    {0xAAE0, 0xAAEA}, {0xABC0, 0xABDA}, {0x10A00, 0x10A00},
    {0x10A10, 0x10A13}, {0x10A15, 0x10A17}, {0x10A19, 0x10A35},
    {0x11103, 0x11126}, {0x11144, 0x11144}, {0x11147, 0x11147},
    {0x11380, 0x11389}, {0x1138B, 0x1138B}, {0x1138E, 0x1138E},
    {0x11390, 0x113B5}, {0x11900, 0x11906}, {0x11909, 0x11909},
    {0x1190C, 0x11913}, {0x11915, 0x11916}, {0x11918, 0x1192F},
    {0x11A00, 0x11A00}, {0x11A0B, 0x11A32}, {0x11A50, 0x11A50},
    {0x11A5C, 0x11A83}, {0x11F04, 0x11F10}, {0x11F12, 0x11F33},
};
constexpr Range kExcludedIncbLinker[] = {
    {0x94D, 0x94D},    {0x9CD, 0x9CD},    {0xACD, 0xACD},
    {0xB4D, 0xB4D},    {0xC4D, 0xC4D},    {0xD4D, 0xD4D},
    {0x1039, 0x1039},  {0x17D2, 0x17D2},  {0x1A60, 0x1A60},
    {0x1B44, 0x1B44},  {0x1BAB, 0x1BAB},  {0xA9C0, 0xA9C0},
    {0xAAF6, 0xAAF6},  {0x10A3F, 0x10A3F}, {0x11133, 0x11133},
    {0x113D0, 0x113D0}, {0x1193E, 0x1193E}, {0x11A47, 0x11A47},
    {0x11A99, 0x11A99}, {0x11F42, 0x11F42},
};

template <std::size_t N>
bool in_ranges(const Range (&ranges)[N], char32_t cp) noexcept {
    for (const Range& r : ranges) {
        if (cp >= r.lo && cp <= r.hi) return true;
    }
    return false;
}

// GB9b（Prepend）或可能触发 GB9c（Consonant + Linker 同现）的用例排除。
bool excluded_case(const std::vector<char32_t>& cps) noexcept {
    bool consonant = false;
    bool linker = false;
    for (char32_t cp : cps) {
        if (in_ranges(kExcludedPrepend, cp)) return true;
        if (in_ranges(kExcludedIncbConsonant, cp)) consonant = true;
        if (in_ranges(kExcludedIncbLinker, cp)) linker = true;
    }
    return consonant && linker;
}

std::string describe(const std::vector<char32_t>& cps) {
    std::string s;
    char buf[8];
    for (char32_t cp : cps) {
        std::snprintf(buf, sizeof buf, "%04X ", static_cast<unsigned>(cp));
        s += buf;
    }
    return s;
}

std::string encode(char32_t cp) {
    char buf[4];
    const std::size_t n = unicode::encode_utf8(cp, buf);
    return std::string(buf, n);
}

} // namespace

BOOST_AUTO_TEST_SUITE(grapheme)

// Unicode 官方一致性：除 GB9b/GB9c 清单外的每条用例，逐码点断点一致。
BOOST_AUTO_TEST_CASE(grapheme_break_test_conformance) {
    std::ifstream in(std::string(DAGENT_TEST_DATA_DIR) +
                     "/GraphemeBreakTest.txt");
    BOOST_REQUIRE(in);
    std::size_t total = 0;
    std::size_t excluded = 0;
    std::size_t checked = 0;
    std::size_t failed = 0;
    std::vector<std::string> failures;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::vector<char32_t> cps;
        std::vector<bool> expect; // 每个码点前是否有断点
        std::istringstream ss(line);
        std::string tok;
        bool break_before = true;
        while (ss >> tok) {
            if (tok == "\xC3\xB7") { // ÷ U+00F7
                break_before = true;
            } else if (tok == "\xC3\x97") { // × U+00D7
                break_before = false;
            } else {
                cps.push_back(
                    static_cast<char32_t>(std::stoul(tok, nullptr, 16)));
                expect.push_back(break_before);
            }
        }
        if (cps.empty() || !expect.front()) continue;
        ++total;
        if (excluded_case(cps)) {
            ++excluded;
            continue;
        }
        ++checked;

        std::string text;
        for (char32_t cp : cps) text += encode(cp);
        std::string_view sv = text;
        std::vector<bool> got;
        while (!sv.empty()) {
            unicode::Grapheme g;
            if (!unicode::next_grapheme(sv, g)) break;
            std::string_view cluster = g.bytes;
            bool first = true;
            while (!cluster.empty()) {
                unicode::decode_utf8(cluster);
                got.push_back(first);
                first = false;
            }
        }
        if (got != expect) {
            ++failed;
            if (failures.size() < 5) failures.push_back(describe(cps));
        }
    }
    for (const std::string& f : failures) {
        BOOST_TEST_MESSAGE("grapheme break mismatch: " << f);
    }
    BOOST_TEST(total > 700);
    BOOST_TEST(checked > 600);
    BOOST_TEST(excluded > 50);
    BOOST_TEST(failed == 0);
}

// emoji 宽度（§3.13）：Emoji_Presentation=Yes 或后随 VS16 的簇为 2 列。
BOOST_AUTO_TEST_CASE(emoji_presentation_and_vs16_widths) {
    BOOST_TEST(unicode::char_width(U'a') == 1);
    BOOST_TEST(unicode::char_width(0x4E2D) == 2);   // 中
    BOOST_TEST(unicode::char_width(0x0301) == 0);   // 组合记号
    BOOST_TEST(unicode::char_width(0x1F600) == 2);  // Emoji_Presentation
    BOOST_TEST(unicode::char_width(0x2764) == 1);   // ❤ 文本呈现

    const auto cluster_width = [](std::string_view s) {
        unicode::Grapheme g;
        BOOST_REQUIRE(unicode::next_grapheme(s, g));
        BOOST_REQUIRE(s.empty()); // 整串必须是一个簇
        return g.width;
    };
    BOOST_TEST(cluster_width("\xE2\x9D\xA4\xEF\xB8\x8F") == 2);       // ❤️
    BOOST_TEST(cluster_width("#\xEF\xB8\x8F\xE2\x83\xA3") == 2);      // #️⃣
    BOOST_TEST(cluster_width("\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3") == 2); // 🇨🇳
    BOOST_TEST(
        cluster_width("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
                      "\xE2\x80\x8D\xF0\x9F\x91\xA7") == 2); // 👨‍👩‍👧
}

// intern 表上限（§3.13）：连续写入 5000 个不同的 ZWJ 序列；溢出时清表并
// 整屏重画，表项数始终有界，且重画后每格仍还原出正确的字素。
BOOST_AUTO_TEST_CASE(intern_table_is_capped_and_repaint_stays_consistent) {
    constexpr int k_cols = 40; // 每两个列放一个宽字素
    constexpr int k_rows = 10;
    constexpr int k_per_row = k_cols / 2;
    constexpr int k_cells = k_per_row * k_rows;
    constexpr std::size_t k_total = 5000;

    Surface back(k_cols, k_rows);
    std::vector<std::string> expected(static_cast<std::size_t>(k_cells));

    // 单簇 ZWJ 序列：ExtPict + ZWJ + ExtPict（GB11），字节数 > 4 走 intern。
    const auto sequence = [](std::size_t n) {
        char buf[4];
        std::string s;
        const auto a = static_cast<char32_t>(0x1F400 + n % 254);
        const auto b = static_cast<char32_t>(0x1F550 + (n / 254) % 24);
        std::size_t len = unicode::encode_utf8(a, buf);
        s.append(buf, len);
        len = unicode::encode_utf8(0x200D, buf);
        s.append(buf, len);
        len = unicode::encode_utf8(b, buf);
        s.append(buf, len);
        return s;
    };
    const auto paint = [&] {
        for (int i = 0; i < k_cells; ++i) {
            back.put((i % k_per_row) * 2, i / k_per_row,
                     expected[static_cast<std::size_t>(i)], Style{});
        }
    };

    std::size_t written = 0;
    while (written < k_total) {
        for (int i = 0; i < k_cells; ++i) {
            expected[static_cast<std::size_t>(i)] =
                written < k_total ? sequence(written++)
                                  : std::string("x");
        }
        back.clear();
        paint();
        // L7 的帧间处理：溢出 → 清表 + 强制整屏重画。
        if (intern_overflowed()) {
            intern_reset();
            back.clear();
            paint();
        }
        for (int i = 0; i < k_cells; ++i) {
            const std::string& want = expected[static_cast<std::size_t>(i)];
            const std::string_view got =
                back.at((i % k_per_row) * 2, i / k_per_row).grapheme();
            if (got != want) {
                BOOST_TEST_CONTEXT("cell " << i) { BOOST_TEST(got == want); }
                return;
            }
        }
        BOOST_TEST(intern_size() <= k_intern_max);
    }
    BOOST_TEST(written == k_total);
    BOOST_TEST(intern_size() <= k_intern_max);
}

BOOST_AUTO_TEST_SUITE_END()
