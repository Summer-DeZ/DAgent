#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "tui/grapheme.hpp"

using namespace dagent::tui::unicode;

namespace {

struct Split {
    std::string bytes;
    int width;
    bool operator==(const Split&) const = default;
};

std::vector<Split> split(std::string_view s) {
    std::vector<Split> out;
    Grapheme g;
    while (next_grapheme(s, g)) out.push_back({std::string(g.bytes), g.width});
    return out;
}

// char32_t 不可打印：以数值比较，失败时能看到码点。
constexpr std::uint32_t code(char32_t c) noexcept { return c; }

} // namespace

BOOST_AUTO_TEST_SUITE(grapheme)

BOOST_AUTO_TEST_CASE(char_width_classes) {
    BOOST_TEST(char_width(U'a') == 1);
    BOOST_TEST(char_width(U'中') == 2);
    BOOST_TEST(char_width(U'가') == 2);
    BOOST_TEST(char_width(U'Ａ') == 2);                // 全角拉丁
    BOOST_TEST(char_width(U'\U00000301') == 0);        // 组合重音
    BOOST_TEST(char_width(U'\U0000200B') == 0);        // 零宽空格
    BOOST_TEST(char_width(U'\t') == 0);
    BOOST_TEST(char_width(U'\U0001F600') == 2);        // 😀
    BOOST_TEST(char_width(U'\U00020000') == 2);        // CJK 扩展 B
    BOOST_TEST(char_width(U'─') == 1);
}

BOOST_AUTO_TEST_CASE(decode_valid_sequences) {
    std::string_view s = "a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80";  // a é 中 😀
    BOOST_TEST(code(decode_utf8(s)) == code(U'a'));
    BOOST_TEST(code(decode_utf8(s)) == code(U'é'));
    BOOST_TEST(code(decode_utf8(s)) == code(U'中'));
    BOOST_TEST(code(decode_utf8(s)) == code(U'\U0001F600'));
    BOOST_TEST(s.empty());
}

BOOST_AUTO_TEST_CASE(decode_invalid_never_swallows_valid_bytes) {
    // 非法、过长编码、代理区、中途断开：逐字节替换为 U+FFFD，其后的 'a' 必须保留
    for (std::string_view bad : {"\xFF" "a", "\xC0\x80" "a", "\xED\xA0\x80" "a", "\xE4" "ab"}) {
        std::string_view s = bad;
        while (!s.empty() && static_cast<unsigned char>(s[0]) >= 0x80) {
            BOOST_TEST(code(decode_utf8(s)) == code(U'\U0000FFFD'));
        }
        BOOST_TEST(s.substr(0, 1) == "a");
    }
}

BOOST_AUTO_TEST_CASE(decode_truncated_sequence_keeps_following_ascii) {
    std::string_view s = "\xE4" "a";  // 三字节首字节后紧跟 ASCII，且到达末尾
    BOOST_TEST(code(decode_utf8(s)) == code(U'\U0000FFFD'));
    BOOST_TEST(s == "a");

    std::string_view tail = "\xE4\xB8";  // 末尾截断的多字节序列整体作废
    BOOST_TEST(code(decode_utf8(tail)) == code(U'\U0000FFFD'));
    BOOST_TEST(tail.empty());
}

BOOST_AUTO_TEST_CASE(encode_roundtrip) {
    for (char32_t cp : {U'a', U'é', U'中', U'\U0001F600', U'\U0010FFFF'}) {
        char buf[4];
        const std::size_t n = encode_utf8(cp, buf);
        std::string_view s(buf, n);
        BOOST_TEST(code(decode_utf8(s)) == code(cp));
        BOOST_TEST(s.empty());
    }
}

BOOST_AUTO_TEST_CASE(clusters) {
    BOOST_TEST(split("e\U00000301x") == (std::vector<Split>{{"e\U00000301", 1}, {"x", 1}}));
    // ZWJ 家族序列：一个簇，宽 2
    BOOST_TEST(split("\U0001F468\U0000200D\U0001F469\U0000200D\U0001F467") ==
               (std::vector<Split>{{"\U0001F468\U0000200D\U0001F469\U0000200D\U0001F467", 2}}));
    // 国旗：区域指示符两两成对
    BOOST_TEST(split("\U0001F1E8\U0001F1F3\U0001F1FA\U0001F1F8") ==
               (std::vector<Split>{{"\U0001F1E8\U0001F1F3", 2}, {"\U0001F1FA\U0001F1F8", 2}}));
    // 肤色修饰附着到前一簇
    BOOST_TEST(split("\U0001F44D\U0001F3FD") == (std::vector<Split>{{"\U0001F44D\U0001F3FD", 2}}));
    // VS16 把文本呈现的符号提为宽 2；ASCII 数字不受影响
    BOOST_TEST(split("\U00002764\U0000FE0F") == (std::vector<Split>{{"\U00002764\U0000FE0F", 2}}));
    BOOST_TEST(split("1\U0000FE0F") == (std::vector<Split>{{"1\U0000FE0F", 1}}));
    // 零宽格式字符自成零宽簇
    BOOST_TEST(split("a\U0000200Bb") ==
               (std::vector<Split>{{"a", 1}, {"\U0000200B", 0}, {"b", 1}}));
}

BOOST_AUTO_TEST_SUITE_END()
