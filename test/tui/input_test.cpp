// L6 验收（02-tui-final-update §3.2）：终端字符串 / 应答窗口与 kitty
// 键盘协议。解码器是纯字节状态机，这里直接喂字节、断言事件流，
// 不做任何 pty 或线程模拟。
//
// 分块不变性：解码器对同一字节流的整喂 / 逐字节 / 随机分块必须产出
// 相同的事件序列。同一次 feed 内连续可打印字符会合并成一个 text 事件，
// 因此比较前先把相邻 text 合并（文档 §9.1 的约定：约束"拼接后的文本"，
// 不是事件边界）。

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "tui/input.hpp"

// Boost.Test 断言失败时要打印枚举，测试 TU 内补上流输出。
namespace dagent::tui {
inline std::ostream& operator<<(std::ostream& os, Mods m) {
    return os << "Mods(" << static_cast<int>(m) << ")";
}
inline std::ostream& operator<<(std::ostream& os, Key k) {
    return os << "Key(" << static_cast<int>(k) << ")";
}
inline std::ostream& operator<<(std::ostream& os, Event::Kind k) {
    return os << "Kind(" << static_cast<int>(k) << ")";
}
inline std::ostream& operator<<(std::ostream& os, Event::ReplyType t) {
    return os << "ReplyType(" << static_cast<int>(t) << ")";
}
} // namespace dagent::tui

using namespace dagent::tui;

namespace {

bool same_event(const Event& a, const Event& b) {
    return a.kind == b.kind && a.text == b.text && a.key == b.key &&
           a.mods == b.mods && a.reply_type == b.reply_type &&
           a.mouse.button == b.mouse.button && a.mouse.col == b.mouse.col &&
           a.mouse.row == b.mouse.row && a.mouse.press == b.mouse.press &&
           a.mouse.motion == b.mouse.motion && a.size == b.size &&
           a.focus_gained == b.focus_gained;
}

// 合并相邻 text 事件，使整喂与分块喂的结果可比。
std::vector<Event> canonical(std::vector<Event> ev) {
    std::vector<Event> out;
    for (Event& e : ev) {
        if (e.kind == Event::Kind::text && !out.empty() &&
            out.back().kind == Event::Kind::text) {
            out.back().text += e.text;
        } else {
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::vector<Event> decode(std::string_view bytes,
                          const std::vector<std::size_t>& chunks,
                          bool reply_window) {
    Decoder d;
    if (reply_window) d.set_reply_window(true);
    std::vector<Event> out;
    std::size_t pos = 0;
    for (std::size_t len : chunks) {
        if (pos >= bytes.size()) break;
        len = std::min(len, bytes.size() - pos);
        d.feed(bytes.substr(pos, len), out);
        pos += len;
    }
    d.flush_escape(out);
    return out;
}

constexpr std::size_t k_mib = 1u << 20;

// ESC 密集的模糊字母表：§3.2 验收要求加入 ] P \ u : BEL。
const std::string k_fuzz_alphabet =
    "\x1b\x1b\x1b[]O]P_^X\\u:;"
    "\x07"
    "abcABC019~Mm?>=<$"
    "\r\n\t\x7f";

std::string random_input(std::mt19937& rng) {
    std::uniform_int_distribution<std::size_t> len(1, 96);
    std::uniform_int_distribution<std::size_t> pick(0, k_fuzz_alphabet.size() - 1);
    std::string s;
    s.reserve(96);
    for (std::size_t i = 0, n = len(rng); i < n; ++i) {
        s.push_back(k_fuzz_alphabet[pick(rng)]);
    }
    return s;
}

} // namespace

BOOST_AUTO_TEST_SUITE(input)

BOOST_AUTO_TEST_CASE(reply_window_decodes_osc_strings) {
    // 验收：OSC 11 的 ST 与 BEL 两种终止各产出恰好一个 reply，
    // 零 text/key；残缺的字符串不参与 Esc 超时。
    for (const std::string& seq :
         {std::string("\x1b]11;rgb:1e1e/1e1e/1e1e\x1b\\"),
          std::string("\x1b]11;rgb:1e1e/1e1e/1e1e\x07")}) {
        Decoder d;
        d.set_reply_window(true);
        std::vector<Event> out;
        d.feed(seq, out);
        BOOST_REQUIRE_EQUAL(out.size(), 1);
        BOOST_TEST(out[0].kind == Event::Kind::reply);
        BOOST_TEST(out[0].reply_type == Event::ReplyType::osc);
        BOOST_TEST(out[0].text == "11;rgb:1e1e/1e1e/1e1e");
        BOOST_TEST(!d.pending_escape());
    }
}

BOOST_AUTO_TEST_CASE(reply_window_decodes_dcs_apc_pm_sos) {
    // DCS XTVERSION 应答不再泄漏成按键；APC/PM/SOS 同规则（仅 ST 终止）。
    Decoder d;
    d.set_reply_window(true);
    std::vector<Event> out;

    d.feed("\x1bP>|XTerm(370)\x1b\\", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].kind == Event::Kind::reply);
    BOOST_TEST(out[0].reply_type == Event::ReplyType::dcs);
    BOOST_TEST(out[0].text == ">|XTerm(370)");

    out.clear();
    d.feed("\x1b_Gi=1;OK\x1b\\", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].reply_type == Event::ReplyType::apc);
    BOOST_TEST(out[0].text == "Gi=1;OK");

    out.clear();
    d.feed("\x1b^private\x1b\\", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].kind == Event::Kind::reply);
    BOOST_TEST(out[0].text == "private");

    out.clear();
    d.feed("\x1bXstart\x1b\\", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].kind == Event::Kind::reply);
    BOOST_TEST(out[0].text == "start");
}

BOOST_AUTO_TEST_CASE(reply_window_decodes_private_csi) {
    // 验收：DA1、DECRQM、kitty 查询应答各产出恰好一个 reply，
    // 零 text/key。text 是完整序列体（含私有标记与最终字节）。
    Decoder d;
    d.set_reply_window(true);
    std::vector<Event> out;

    d.feed("\x1b[?2026;2$y", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].kind == Event::Kind::reply);
    BOOST_TEST(out[0].reply_type == Event::ReplyType::csi);
    BOOST_TEST(out[0].text == "?2026;2$y");

    out.clear();
    d.feed("\x1b[?62;22c", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].text == "?62;22c");

    out.clear();
    d.feed("\x1b[?1u", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].text == "?1u");

    out.clear();
    d.feed("\x1b[>0;276;0c", out); // 次级 DA 也是应答
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].text == ">0;276;0c");
}

BOOST_AUTO_TEST_CASE(reply_string_survives_chunked_feeds) {
    // 字符串跨读取边界：任意切分位置产出同一个 reply。
    const std::string seq = "\x1b]11;rgb:1e1e/1e1e/1e1e\x1b\\";
    for (std::size_t split = 1; split < seq.size(); ++split) {
        Decoder d;
        d.set_reply_window(true);
        std::vector<Event> out;
        d.feed(std::string_view(seq).substr(0, split), out);
        BOOST_TEST(out.empty());
        d.feed(std::string_view(seq).substr(split), out);
        BOOST_REQUIRE_EQUAL(out.size(), 1);
        BOOST_TEST(out[0].kind == Event::Kind::reply);
        BOOST_TEST(out[0].text == "11;rgb:1e1e/1e1e/1e1e");
    }
}

BOOST_AUTO_TEST_CASE(reply_window_closed_keeps_alt_semantics) {
    // 验收：窗口关闭时 \e]x 仍是 Alt-] + x；私有 CSI 整体丢弃。
    Decoder d;
    std::vector<Event> out;
    d.feed("\x1b]x", out);
    BOOST_REQUIRE_EQUAL(out.size(), 2);
    BOOST_TEST(out[0].kind == Event::Kind::key);
    BOOST_TEST(out[0].key == Key::none);
    BOOST_TEST(any(out[0].mods & Mods::alt));
    BOOST_TEST(out[0].text == "]");
    BOOST_TEST(out[1].kind == Event::Kind::text);
    BOOST_TEST(out[1].text == "x");

    out.clear();
    d.feed("\x1b[?62;22c", out);
    BOOST_TEST(out.empty());

    // 窗口打开又关闭后必须回到原语义，不能残留状态。
    d.set_reply_window(true);
    d.set_reply_window(false);
    out.clear();
    d.feed("\x1b]y", out);
    BOOST_REQUIRE_EQUAL(out.size(), 2);
    BOOST_TEST(out[0].text == "]");
    BOOST_TEST(out[1].text == "y");
}

BOOST_AUTO_TEST_CASE(reply_window_close_drops_unterminated_reply) {
    // 窗口内残缺的字符串/私有 CSI 不参与 40ms Esc 超时；窗口关闭时
    // 整体丢弃，后续用户输入不受污染。
    {
        Decoder d;
        d.set_reply_window(true);
        std::vector<Event> out;
        d.feed("\x1b]11;rgb:1e1e", out);
        BOOST_TEST(out.empty());
        BOOST_TEST(!d.pending_escape());
        d.flush_escape(out); // 超时消解不得把残缺应答变成按键
        BOOST_TEST(out.empty());

        d.set_reply_window(false);
        d.feed("ok", out);
        BOOST_REQUIRE_EQUAL(out.size(), 1);
        BOOST_TEST(out[0].kind == Event::Kind::text);
        BOOST_TEST(out[0].text == "ok");
    }
    {
        Decoder d;
        d.set_reply_window(true);
        std::vector<Event> out;
        d.feed("\x1b[?2026;2", out); // 还差 $y
        BOOST_TEST(out.empty());
        BOOST_TEST(!d.pending_escape());
        d.set_reply_window(false);
        d.feed("z", out);
        BOOST_REQUIRE_EQUAL(out.size(), 1);
        BOOST_TEST(out[0].text == "z");
    }
}

BOOST_AUTO_TEST_CASE(reply_string_is_capped_at_one_mib) {
    Decoder d;
    d.set_reply_window(true);
    std::string seq = "\x1b]52;c;";
    seq.append(k_mib + 64, 'A');
    seq.push_back('\x07');
    std::vector<Event> out;
    d.feed(seq, out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].kind == Event::Kind::reply);
    BOOST_TEST(out[0].reply_type == Event::ReplyType::osc);
    BOOST_TEST(out[0].text.size() == k_mib); // 超出部分丢弃，仍读到终止符
}

BOOST_AUTO_TEST_CASE(kitty_key_decoding) {
    // 验收：13;2u → enter+shift；97;5u → key ctrl + "a"；1;5:1A → up+ctrl。
    Decoder d;
    std::vector<Event> out;

    d.feed("\x1b[13;2u", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].kind == Event::Kind::key);
    BOOST_TEST(out[0].key == Key::enter);
    BOOST_TEST(out[0].mods == Mods::shift);
    BOOST_TEST(out[0].text.empty());

    out.clear();
    d.feed("\x1b[97;5u", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].kind == Event::Kind::key);
    BOOST_TEST(out[0].key == Key::none);
    BOOST_TEST(out[0].mods == Mods::ctrl);
    BOOST_TEST(out[0].text == "a");

    out.clear();
    d.feed("\x1b[1;5:1A", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].kind == Event::Kind::key);
    BOOST_TEST(out[0].key == Key::up);
    BOOST_TEST(out[0].mods == Mods::ctrl);

    // 无修饰的可打印码点走文本路径；命名码点映射按文档。
    out.clear();
    d.feed("\x1b[97u", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].kind == Event::Kind::text);
    BOOST_TEST(out[0].text == "a");

    out.clear();
    d.feed("\x1b[27u", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].key == Key::escape);

    out.clear();
    d.feed("\x1b[9;2u", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].key == Key::tab);
    BOOST_TEST(out[0].mods == Mods::shift);

    out.clear();
    d.feed("\x1b[127;2u", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].key == Key::backspace);
    BOOST_TEST(out[0].mods == Mods::shift);

    // super 位；hyper/meta/caps/num 忽略；shift 的 shifted key 生效；
    // release 事件（flag 2 的事，这里只是兼容）丢弃。
    out.clear();
    d.feed("\x1b[97;9u", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].mods == Mods::super);

    out.clear();
    d.feed("\x1b[97;66u", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].mods == Mods::shift); // caps 位被忽略

    out.clear();
    d.feed("\x1b[97:65;2u", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].kind == Event::Kind::key);
    BOOST_TEST(out[0].mods == Mods::shift);
    BOOST_TEST(out[0].text == "A");

    out.clear();
    d.feed("\x1b[13;5:3u", out);
    BOOST_TEST(out.empty());

    // 带修饰的 ~ 终止键（xterm 风格）与残缺子参数不受影响。
    out.clear();
    d.feed("\x1b[3;5:1~", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].key == Key::del);
    BOOST_TEST(out[0].mods == Mods::ctrl);

    // 窗口打开不应影响普通键入。
    out.clear();
    d.set_reply_window(true);
    d.feed("hi", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].kind == Event::Kind::text);
    BOOST_TEST(out[0].text == "hi");
}

BOOST_AUTO_TEST_CASE(legacy_csi_keys_mouse_and_paste_still_decode) {
    // 3.2 重写的是同一个 CSI 解析器：原有语义（箭头/修饰键/功能键、
    // SGR 与 X10 鼠标、括号粘贴、孤立 ESC 超时）必须原样保留。
    Decoder d;
    std::vector<Event> out;

    d.feed("\x1b[A\x1b[1;2A\x1b[3~", out);
    BOOST_REQUIRE_EQUAL(out.size(), 3);
    BOOST_TEST(out[0].key == Key::up);
    BOOST_TEST(out[1].key == Key::up);
    BOOST_TEST(out[1].mods == Mods::shift);
    BOOST_TEST(out[2].key == Key::del);

    out.clear();
    d.feed("\x1b[<0;10;5M\x1b[<0;10;5m", out);
    BOOST_REQUIRE_EQUAL(out.size(), 2);
    BOOST_TEST(out[0].kind == Event::Kind::mouse);
    BOOST_TEST(out[0].mouse.button == 0);
    BOOST_TEST(out[0].mouse.col == 9);
    BOOST_TEST(out[0].mouse.row == 4);
    BOOST_TEST(out[0].mouse.press);
    BOOST_TEST(out[1].kind == Event::Kind::mouse);
    BOOST_TEST(!out[1].mouse.press);

    out.clear();
    d.feed("\x1b[Mabc", out); // X10：负载整体吞掉，不产出事件
    BOOST_TEST(out.empty());

    out.clear();
    d.feed("\x1b[200~abc\x1b[201~", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].kind == Event::Kind::paste);
    BOOST_TEST(out[0].text == "abc");

    out.clear();
    d.feed("\x1b\x1b[A", out); // rxvt：Alt-Up（倒数第二个 ESC 是 Alt 前缀）
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].key == Key::up);
    BOOST_TEST(out[0].mods == Mods::alt);

    out.clear();
    d.feed("\x1b\x1b\x1b[A", out); // 多余 ESC 才是 Esc 按键
    BOOST_REQUIRE_EQUAL(out.size(), 2);
    BOOST_TEST(out[0].key == Key::escape);
    BOOST_TEST(out[1].key == Key::up);
    BOOST_TEST(out[1].mods == Mods::alt);

    out.clear();
    d.feed("\x1b", out);
    BOOST_TEST(d.pending_escape());
    d.flush_escape(out);
    BOOST_REQUIRE_EQUAL(out.size(), 1);
    BOOST_TEST(out[0].key == Key::escape);
}

BOOST_AUTO_TEST_CASE(esc_dense_fuzz_is_chunking_invariant) {
    // 验收：字母表含 ] P \ u : BEL 的 ESC 密集模糊测试，整喂 /
    // 逐字节 / 随机分块结果一致。应答窗口开与关两种状态都跑。
    std::mt19937 rng(0x5eed);
    for (int iter = 0; iter < 400; ++iter) {
        const std::string bytes = random_input(rng);
        for (const bool window : {false, true}) {
            const std::vector<Event> ref =
                canonical(decode(bytes, {bytes.size()}, window));
            const std::vector<Event> got = canonical(decode(
                bytes, std::vector<std::size_t>(bytes.size(), 1), window));

            BOOST_REQUIRE_EQUAL(got.size(), ref.size());
            for (std::size_t i = 0; i < ref.size(); ++i) {
                BOOST_TEST(same_event(got[i], ref[i]));
            }

            // 随机分块：1..7 字节一段。
            std::vector<std::size_t> chunks;
            std::uniform_int_distribution<std::size_t> part(1, 7);
            for (std::size_t pos = 0; pos < bytes.size();) {
                const std::size_t n = std::min(part(rng), bytes.size() - pos);
                chunks.push_back(n);
                pos += n;
            }
            const std::vector<Event> got2 = canonical(decode(bytes, chunks, window));
            BOOST_REQUIRE_EQUAL(got2.size(), ref.size());
            for (std::size_t i = 0; i < ref.size(); ++i) {
                BOOST_TEST(same_event(got2[i], ref[i]));
            }
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
