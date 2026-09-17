// L6 输入层验证（文档§13.5）：随机/截断字节流不崩溃、不吞掉后续输入、
// 不产生伪造事件；分块不变性；焦点链路由顺序；InputBox 按键翻译。

#include <boost/test/unit_test.hpp>

#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "tui/input.hpp"
#include "tui/widget.hpp"

using namespace dagent::tui;

// BOOST_CHECK 直接比较枚举时失败信息需要可打印（support.hpp 未覆盖本层）。
namespace dagent::tui {

inline std::ostream& operator<<(std::ostream& os, Event::Kind k) {
    switch (k) {
    case Event::Kind::text: return os << "text";
    case Event::Kind::key: return os << "key";
    case Event::Kind::mouse: return os << "mouse";
    case Event::Kind::paste: return os << "paste";
    case Event::Kind::resize: return os << "resize";
    case Event::Kind::focus: return os << "focus";
    }
    return os;
}

inline std::ostream& operator<<(std::ostream& os, Mods m) {
    return os << "Mods{" << static_cast<int>(m) << "}";
}

} // namespace dagent::tui

namespace {

// 一次性喂完整字节流（末尾附带孤立 ESC 的歧义消解）。
std::vector<Event> decode_all(std::string_view bytes) {
    Decoder d;
    std::vector<Event> out;
    d.feed(bytes, out);
    d.flush_escape(out);
    return out;
}

// 按 1-7 字节的随机分块喂入：解码结果必须与整喂一致。
std::vector<Event> decode_split(std::string_view bytes, std::mt19937& rng) {
    Decoder d;
    std::vector<Event> out;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::size_t n = std::uniform_int_distribution<std::size_t>(1, 7)(rng);
        if (pos + n > bytes.size()) n = bytes.size() - pos;
        d.feed(bytes.substr(pos, n), out);
        pos += n;
    }
    d.flush_escape(out);
    return out;
}

std::vector<Event> decode_bytewise(std::string_view bytes) {
    Decoder d;
    std::vector<Event> out;
    for (const char b : bytes) d.feed(std::string_view(&b, 1), out);
    d.flush_escape(out);
    return out;
}

bool same(const Event& a, const Event& b) {
    if (a.kind != b.kind) return false;
    if (a.text != b.text || a.key != b.key || a.mods != b.mods) return false;
    if (a.kind == Event::Kind::mouse) {
        return a.mouse.button == b.mouse.button && a.mouse.col == b.mouse.col &&
               a.mouse.row == b.mouse.row && a.mouse.press == b.mouse.press &&
               a.mouse.motion == b.mouse.motion;
    }
    if (a.kind == Event::Kind::focus) return a.focus_gained == b.focus_gained;
    return true;
}

bool same_stream(const std::vector<Event>& a, const std::vector<Event>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!same(a[i], b[i])) return false;
    }
    return true;
}

// 同一次 feed 内的连续文本会合并成一个 text 事件（§二），
// 分块不变性因此约束"拼接后的文本"：比较前先合并相邻 text 事件。
std::vector<Event> merge_text(std::vector<Event> es) {
    std::vector<Event> out;
    for (auto& e : es) {
        if (e.kind == Event::Kind::text && !out.empty() &&
            out.back().kind == Event::Kind::text) {
            out.back().text += e.text;
        } else {
            out.push_back(std::move(e));
        }
    }
    return out;
}

bool same_text_stream(const std::vector<Event>& a, const std::vector<Event>& b) {
    return same_stream(merge_text(a), merge_text(b));
}

Event text_ev(std::string s) {
    Event e;
    e.kind = Event::Kind::text;
    e.text = std::move(s);
    return e;
}

Event key_ev(Key k, Mods m = Mods::none, std::string ch = {}) {
    Event e;
    e.kind = Event::Kind::key;
    e.key = k;
    e.mods = m;
    e.text = std::move(ch);
    return e;
}

// 单元格事件辅助：取 events 的第 i 个事件并断言存在。
const Event& at(const std::vector<Event>& es, std::size_t i) {
    BOOST_REQUIRE(i < es.size());
    return es[i];
}

// 路由测试用：按名字记录"谁消费了事件"。
struct Recorder : EventHandler {
    bool on_event(const Event&) override {
        hits.push_back(this);
        return consume;
    }
    std::vector<const EventHandler*> hits;
    bool consume = true;
};

} // namespace

BOOST_AUTO_TEST_SUITE(input_suite)

BOOST_AUTO_TEST_CASE(text_merged_per_feed) {
    // 同一次 feed 内的连续文本合并为一个事件（大段输入只触发一次插入）。
    const auto es = decode_all("hi你");
    BOOST_REQUIRE_EQUAL(es.size(), 1u);
    BOOST_CHECK(same(at(es, 0), text_ev("hi你")));
}

BOOST_AUTO_TEST_CASE(utf8_split_across_feeds) {
    // 多字节字符从任意字节处切开，拼回后按 feed 边界出事件。
    Decoder d;
    std::vector<Event> out;
    d.feed("a\xE4", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1u);
    BOOST_CHECK(same(at(out, 0), text_ev("a")));
    d.feed("\xBD\xA0" "b", out);
    BOOST_REQUIRE_EQUAL(out.size(), 2u);
    BOOST_CHECK(same(at(out, 1), text_ev("你b")));
}

BOOST_AUTO_TEST_CASE(invalid_utf8_dropped_never_forged) {
    // 非法字节逐个丢弃，前后文本照常合并，不产生 U+FFFD 事件。
    const auto es = decode_all("a\xFF" "b");
    BOOST_REQUIRE_EQUAL(es.size(), 1u);
    BOOST_CHECK(same(at(es, 0), text_ev("ab")));

    // 过长编码与代理区同样丢弃。
    BOOST_CHECK(decode_all("\xC0\xAF").empty());
    BOOST_CHECK(decode_all("\xED\xA0\x80").empty());
    BOOST_CHECK(decode_all("\xF4\x90\x80\x80").empty());
}

BOOST_AUTO_TEST_CASE(control_keys) {
    const auto enter = decode_all("\r\n");
    BOOST_REQUIRE_EQUAL(enter.size(), 2u);
    BOOST_CHECK(same(at(enter, 0), key_ev(Key::enter)));
    BOOST_CHECK(same(at(enter, 1), key_ev(Key::enter)));

    BOOST_CHECK(same(at(decode_all("\t"), 0), key_ev(Key::tab)));
    BOOST_CHECK(same(at(decode_all("\x7F"), 0), key_ev(Key::backspace)));
    BOOST_CHECK(same(at(decode_all("\x01"), 0), key_ev(Key::none, Mods::ctrl, "a")));
    BOOST_CHECK(same(at(decode_all("\x1A"), 0), key_ev(Key::none, Mods::ctrl, "z")));
    BOOST_CHECK(same(at(decode_all("\x08"), 0), key_ev(Key::none, Mods::ctrl, "h")));
    // "\x00" 字面量经 strlen 衰减会变空：显式给长度。
    BOOST_CHECK(same(at(decode_all(std::string_view("\0", 1)), 0),
                     key_ev(Key::none, Mods::ctrl, " ")));
    BOOST_CHECK(same(at(decode_all("\x1D"), 0), key_ev(Key::none, Mods::ctrl, "]")));
}

BOOST_AUTO_TEST_CASE(csi_keys_with_modifiers) {
    BOOST_CHECK(same(at(decode_all("\e[A"), 0), key_ev(Key::up)));
    BOOST_CHECK(same(at(decode_all("\e[B"), 0), key_ev(Key::down)));
    BOOST_CHECK(same(at(decode_all("\e[C"), 0), key_ev(Key::right)));
    BOOST_CHECK(same(at(decode_all("\e[D"), 0), key_ev(Key::left)));
    BOOST_CHECK(same(at(decode_all("\e[H"), 0), key_ev(Key::home)));
    BOOST_CHECK(same(at(decode_all("\e[F"), 0), key_ev(Key::end)));
    BOOST_CHECK(same(at(decode_all("\e[1;5A"), 0), key_ev(Key::up, Mods::ctrl)));
    BOOST_CHECK(same(at(decode_all("\e[1;3D"), 0), key_ev(Key::left, Mods::alt)));
    BOOST_CHECK(same(at(decode_all("\e[1;2H"), 0), key_ev(Key::home, Mods::shift)));
    BOOST_CHECK(same(at(decode_all("\e[Z"), 0), key_ev(Key::tab, Mods::shift)));

    BOOST_CHECK(same(at(decode_all("\e[2~"), 0), key_ev(Key::insert)));
    BOOST_CHECK(same(at(decode_all("\e[3~"), 0), key_ev(Key::del)));
    BOOST_CHECK(same(at(decode_all("\e[3;5~"), 0), key_ev(Key::del, Mods::ctrl)));
    BOOST_CHECK(same(at(decode_all("\e[5~"), 0), key_ev(Key::page_up)));
    BOOST_CHECK(same(at(decode_all("\e[6~"), 0), key_ev(Key::page_down)));
    BOOST_CHECK(same(at(decode_all("\e[1~"), 0), key_ev(Key::home)));
    BOOST_CHECK(same(at(decode_all("\e[4~"), 0), key_ev(Key::end)));
    BOOST_CHECK(same(at(decode_all("\e[15~"), 0), key_ev(Key::f5)));
    BOOST_CHECK(same(at(decode_all("\e[24~"), 0), key_ev(Key::f12)));
}

BOOST_AUTO_TEST_CASE(ss3_keys) {
    BOOST_CHECK(same(at(decode_all("\eOA"), 0), key_ev(Key::up)));
    BOOST_CHECK(same(at(decode_all("\eOD"), 0), key_ev(Key::left)));
    BOOST_CHECK(same(at(decode_all("\eOH"), 0), key_ev(Key::home)));
    BOOST_CHECK(same(at(decode_all("\eOF"), 0), key_ev(Key::end)));
    BOOST_CHECK(same(at(decode_all("\eOP"), 0), key_ev(Key::f1)));
    BOOST_CHECK(same(at(decode_all("\eOS"), 0), key_ev(Key::f4)));
    BOOST_CHECK(same(at(decode_all("\eOM"), 0), key_ev(Key::enter)));
}

BOOST_AUTO_TEST_CASE(alt_combinations) {
    BOOST_CHECK(same(at(decode_all("\ex"), 0), key_ev(Key::none, Mods::alt, "x")));
    BOOST_CHECK(same(at(decode_all("\e\r"), 0), key_ev(Key::enter, Mods::alt)));
    BOOST_CHECK(same(at(decode_all("\e\x7F"), 0), key_ev(Key::backspace, Mods::alt)));
    BOOST_CHECK(same(at(decode_all("\e你"), 0), key_ev(Key::none, Mods::alt, "你")));

    // 连续 ESC 串 + 序列（bug 5 / 复查 B）：串的最后一个 ESC 是序列自身的
    // 引导符，倒数第二个是 Alt 前缀，再往前的各是一次 Esc 按键。
    // rxvt 的 Alt-Up 就是 \e\e[A：恰好一个 Alt-Up，不能多出 Esc。
    const auto alt_up = decode_all("\e\e[A");
    BOOST_REQUIRE_EQUAL(alt_up.size(), 1u);
    BOOST_CHECK(same(at(alt_up, 0), key_ev(Key::up, Mods::alt)));

    const auto triple = decode_all("\e\e\e[A");
    BOOST_REQUIRE_EQUAL(triple.size(), 2u);
    BOOST_CHECK(same(at(triple, 0), key_ev(Key::escape)));
    BOOST_CHECK(same(at(triple, 1), key_ev(Key::up, Mods::alt)));

    const auto alt_f1 = decode_all("\e\e\eOP");
    BOOST_REQUIRE_EQUAL(alt_f1.size(), 2u);
    BOOST_CHECK(same(at(alt_f1, 0), key_ev(Key::escape)));
    BOOST_CHECK(same(at(alt_f1, 1), key_ev(Key::f1, Mods::alt)));

    const auto esc_esc_x = decode_all("\e\ex");
    BOOST_REQUIRE_EQUAL(esc_esc_x.size(), 2u);
    BOOST_CHECK(same(at(esc_esc_x, 0), key_ev(Key::escape)));
    BOOST_CHECK(same(at(esc_esc_x, 1), key_ev(Key::none, Mods::alt, "x")));
}

BOOST_AUTO_TEST_CASE(unknown_sequences_dropped_completely) {
    // 终端应答（DA）落在 stdin 上：读到终止符整体丢弃，后续输入完好。
    const auto es = decode_all("\e[?1;2cabc");
    BOOST_REQUIRE_EQUAL(es.size(), 1u);
    BOOST_CHECK(same(at(es, 0), text_ev("abc")));

    // 私有标记与中间字节同样按不可识别处理。
    BOOST_CHECK(decode_all("\e[>0q").empty());
    BOOST_CHECK(decode_all("\e[2 q").empty());
    BOOST_CHECK(decode_all("\e[1;2:3~").empty());

    // 序列中途出现 ESC：前缀作废，从新 ESC 重新解析，一个 up 事件。
    const auto interrupted = decode_all("\e[12\e[A");
    BOOST_REQUIRE_EQUAL(interrupted.size(), 1u);
    BOOST_CHECK(same(at(interrupted, 0), key_ev(Key::up)));

    // 修饰参数为 0：等价无修饰（bug 7，之前会置上全部修饰位）。
    BOOST_CHECK(same(at(decode_all("\e[1;0A"), 0), key_ev(Key::up)));

    // CSI 内的 C0 照常执行（bug 6）：序列完整时回车先于序列本身产出，
    // 且不继承序列的 Alt 前缀。
    const auto c0_in_csi = decode_all("\e\e[\rA");
    BOOST_REQUIRE_EQUAL(c0_in_csi.size(), 2u);
    BOOST_CHECK(same(at(c0_in_csi, 0), key_ev(Key::enter)));
    BOOST_CHECK(same(at(c0_in_csi, 1), key_ev(Key::up, Mods::alt)));

    // 引导符后只有 C0 就超时：Alt-[ 后紧跟回车，两个按键都不丢。
    const auto alt_bracket_enter = decode_all("\e[\r");
    BOOST_REQUIRE_EQUAL(alt_bracket_enter.size(), 2u);
    BOOST_CHECK(same(at(alt_bracket_enter, 0), key_ev(Key::none, Mods::alt, "[")));
    BOOST_CHECK(same(at(alt_bracket_enter, 1), key_ev(Key::enter)));

    // 已收参数的残缺序列超时丢弃，其中的 C0 仍然产出。
    const auto partial = decode_all("\e[1;\t5");
    BOOST_REQUIRE_EQUAL(partial.size(), 1u);
    BOOST_CHECK(same(at(partial, 0), key_ev(Key::tab)));

    // CSI 内的 DEL 被忽略，序列继续成立。
    BOOST_CHECK(same(at(decode_all("\e[3\x7F~"), 0), key_ev(Key::del)));

    // CSI 内的 ≥0x80 字节中止序列并按 ground 重新解析。
    const auto nonascii = decode_all("\e[你b");
    BOOST_REQUIRE_EQUAL(nonascii.size(), 1u);
    BOOST_CHECK(same(at(nonascii, 0), text_ev("你b")));
}

BOOST_AUTO_TEST_CASE(ss3_control_bytes) {
    // SS3 与 CSI 同规则（复查遗留）：C0 照常产出、≥0x80 中止序列、DEL 忽略。
    const auto c0 = decode_all("\eO\rA");
    BOOST_REQUIRE_EQUAL(c0.size(), 2u);
    BOOST_CHECK(same(at(c0, 0), key_ev(Key::enter)));
    BOOST_CHECK(same(at(c0, 1), key_ev(Key::up)));

    const auto nonascii = decode_all("\eO你b");
    BOOST_REQUIRE_EQUAL(nonascii.size(), 1u);
    BOOST_CHECK(same(at(nonascii, 0), text_ev("你b")));

    BOOST_CHECK(same(at(decode_all("\eO\x7F" "B"), 0), key_ev(Key::down)));

    // 超时：Alt-O 后紧跟回车。
    const auto alt_o_enter = decode_all("\eO\r");
    BOOST_REQUIRE_EQUAL(alt_o_enter.size(), 2u);
    BOOST_CHECK(same(at(alt_o_enter, 0), key_ev(Key::none, Mods::alt, "O")));
    BOOST_CHECK(same(at(alt_o_enter, 1), key_ev(Key::enter)));
}

BOOST_AUTO_TEST_CASE(control_bytes_in_incomplete_sequence_not_duplicated) {
    // 复查 C：序列未收完时已产出的 C0 必须随等待撤回，否则下一轮重新
    // 解析会再产出一遍。逐字节 / 分三次喂入必须与整喂逐事件一致。
    const std::string seqs[] = {"\e[\rA", "\e[1;\r\n5C", "\eO\t\tP", "\e\e[\x01" "B"};
    for (const auto& s : seqs) {
        const auto whole = decode_all(s);
        BOOST_CHECK(same_stream(whole, decode_bytewise(s)));
    }

    Decoder d;
    std::vector<Event> out;
    d.feed("\e[\r", out);
    d.feed("\r", out);
    d.feed("A", out);
    BOOST_REQUIRE_EQUAL(out.size(), 3u);
    BOOST_CHECK(same(at(out, 0), key_ev(Key::enter)));
    BOOST_CHECK(same(at(out, 1), key_ev(Key::enter)));
    BOOST_CHECK(same(at(out, 2), key_ev(Key::up)));
}

BOOST_AUTO_TEST_CASE(fuzz_chunk_invariance_escape_heavy) {
    // 均匀随机字节里 "ESC [" 的概率只有 1/65536，抓不到序列内部的问题。
    // 这里从转义相关字节里抽样，密集覆盖 ESC 串、引导符、C0、参数与终止符，
    // 并要求逐事件一致（不只是拼接文本一致）。
    const std::string alphabet = std::string("\e\e\e[[O;<M~AP\r\t\x01\x7F" "015") +
                                 "\xE4\xBD\xA0" "a";
    std::mt19937 rng(99);
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    for (int trial = 0; trial < 256; ++trial) {
        std::string s;
        const int n = std::uniform_int_distribution<int>(0, 40)(rng);
        for (int i = 0; i < n; ++i) s += alphabet[pick(rng)];
        BOOST_CHECK(same_text_stream(decode_all(s), decode_bytewise(s)));
        BOOST_CHECK(same_text_stream(decode_all(s), decode_split(s, rng)));
    }
}

BOOST_AUTO_TEST_CASE(x10_legacy_mouse) {
    // 终端不支持 1006 时的旧式 X10 序列：\e[M 后跟 3 个原始字节，
    // 必须连负载整体吞掉，不能漏成文本（bug 4）。
    const auto es = decode_all("\e[M a!z");
    BOOST_REQUIRE_EQUAL(es.size(), 1u);
    BOOST_CHECK(same(at(es, 0), text_ev("z")));

    // 负载跨分块：不足 3 字节时等待，拼齐后同样整体吞掉。
    Decoder d;
    std::vector<Event> out;
    d.feed("\e[M ", out);
    BOOST_CHECK(out.empty());
    d.feed("a!z", out);
    BOOST_REQUIRE_EQUAL(out.size(), 1u);
    BOOST_CHECK(same(at(out, 0), text_ev("z")));
}

BOOST_AUTO_TEST_CASE(escape_timeout_covers_csi_intro) {
    // Alt-[ / Alt-O：引导符后不再有字节，超时判定产出组合键（bug 2）。
    Decoder d;
    std::vector<Event> out;
    d.feed("\e[", out);
    BOOST_CHECK(out.empty());
    BOOST_CHECK(d.pending_escape());
    d.flush_escape(out);
    BOOST_REQUIRE_EQUAL(out.size(), 1u);
    BOOST_CHECK(same(at(out, 0), key_ev(Key::none, Mods::alt, "[")));

    Decoder d2;
    std::vector<Event> out2;
    d2.feed("\eO", out2);
    d2.flush_escape(out2);
    BOOST_REQUIRE_EQUAL(out2.size(), 1u);
    BOOST_CHECK(same(at(out2, 0), key_ev(Key::none, Mods::alt, "O")));

    // 已带参数的残缺序列：超时后整体丢弃。
    Decoder d3;
    std::vector<Event> out3;
    d3.feed("\e[1;5", out3);
    BOOST_CHECK(d3.pending_escape());
    d3.flush_escape(out3);
    BOOST_CHECK(out3.empty());

    // ESC 串 + 引导符：多出的 ESC 先产出 Esc 按键。
    Decoder d4;
    std::vector<Event> out4;
    d4.feed("\e\e[", out4);
    d4.flush_escape(out4);
    BOOST_REQUIRE_EQUAL(out4.size(), 2u);
    BOOST_CHECK(same(at(out4, 0), key_ev(Key::escape)));
    BOOST_CHECK(same(at(out4, 1), key_ev(Key::none, Mods::alt, "[")));
}

BOOST_AUTO_TEST_CASE(escape_disambiguation) {
    // 孤立 ESC：等待窗口内无后续字节 → 单独 Esc 键。
    Decoder d;
    std::vector<Event> out;
    d.feed("\e", out);
    BOOST_CHECK(out.empty());
    BOOST_CHECK(d.pending_escape());
    d.flush_escape(out);
    BOOST_REQUIRE_EQUAL(out.size(), 1u);
    BOOST_CHECK(same(at(out, 0), key_ev(Key::escape)));

    // 窗口内来了后续字节：序列解释优先。
    Decoder d2;
    std::vector<Event> out2;
    d2.feed("\e", out2);
    d2.feed("[A", out2);
    BOOST_REQUIRE_EQUAL(out2.size(), 1u);
    BOOST_CHECK(same(at(out2, 0), key_ev(Key::up)));
    BOOST_CHECK(!d2.pending_escape());

    // 不完整 CSI 同样进入歧义窗口（bug 2）：超时后整体丢弃，不产出事件。
    Decoder d3;
    std::vector<Event> out3;
    d3.feed("\e[2", out3);
    BOOST_CHECK(d3.pending_escape());
    d3.flush_escape(out3);
    BOOST_CHECK(out3.empty());
}

BOOST_AUTO_TEST_CASE(sgr_mouse) {
    const auto press = decode_all("\e[<0;11;5M");
    BOOST_REQUIRE_EQUAL(press.size(), 1u);
    BOOST_CHECK(at(press, 0).mouse.button == 0);
    BOOST_CHECK(at(press, 0).mouse.col == 10); // 1 基 → 0 基
    BOOST_CHECK(at(press, 0).mouse.row == 4);
    BOOST_CHECK(at(press, 0).mouse.press);
    BOOST_CHECK(!at(press, 0).mouse.motion);

    const auto release = decode_all("\e[<0;11;5m");
    BOOST_REQUIRE_EQUAL(release.size(), 1u);
    BOOST_CHECK(!at(release, 0).mouse.press);

    BOOST_CHECK(at(decode_all("\e[<64;1;1M"), 0).mouse.button == 4); // 滚轮上
    BOOST_CHECK(at(decode_all("\e[<65;1;1M"), 0).mouse.button == 5); // 滚轮下

    const auto drag = decode_all("\e[<32;4;7M");
    BOOST_REQUIRE_EQUAL(drag.size(), 1u);
    BOOST_CHECK(at(drag, 0).mouse.button == 0);
    BOOST_CHECK(at(drag, 0).mouse.press);
    BOOST_CHECK(at(drag, 0).mouse.motion);

    const auto hover = decode_all("\e[<35;4;7M");
    BOOST_REQUIRE_EQUAL(hover.size(), 1u);
    BOOST_CHECK(at(hover, 0).mouse.button == -1);
    BOOST_CHECK(at(hover, 0).mouse.motion);

    const auto modified = decode_all("\e[<5;1;1M"); // 4=shift 位 + 按钮 1
    BOOST_REQUIRE_EQUAL(modified.size(), 1u);
    BOOST_CHECK(at(modified, 0).mouse.button == 1);
    BOOST_CHECK(at(modified, 0).mods == Mods::shift);

    BOOST_CHECK(at(decode_all("\e[<16;1;1M"), 0).mods == Mods::ctrl);
}

BOOST_AUTO_TEST_CASE(bracketed_paste) {
    const auto es = decode_all("\e[200~ab\r\ncd\e[201~e");
    BOOST_REQUIRE_EQUAL(es.size(), 2u);
    BOOST_CHECK(at(es, 0).kind == Event::Kind::paste);
    BOOST_CHECK(at(es, 0).text == "ab\r\ncd");
    BOOST_CHECK(same(at(es, 1), text_ev("e")));

    // 任意分块：仍然恰好一个 paste 事件，内容逐字节一致。
    std::mt19937 rng(42);
    const std::string raw = "\e[200~ab\r\ncd\e[201~e";
    BOOST_CHECK(same_stream(decode_all(raw), decode_split(raw, rng)));

    // 内容里的开始标记是数据；结束标记跨分块边界也能拼回来。
    const auto nested = decode_all("\e[200~x\e[200~y\e[201~z");
    BOOST_REQUIRE_EQUAL(nested.size(), 2u);
    BOOST_CHECK(at(nested, 0).text == "x\e[200~y");

    Decoder d;
    std::vector<Event> out;
    d.feed("\e[200~abc\e[20", out);
    d.feed("1~!", out);
    BOOST_REQUIRE_EQUAL(out.size(), 2u);
    BOOST_CHECK(at(out, 0).kind == Event::Kind::paste);
    BOOST_CHECK(at(out, 0).text == "abc");
    BOOST_CHECK(same(at(out, 1), text_ev("!")));
}

BOOST_AUTO_TEST_CASE(focus_events) {
    const auto in = decode_all("\e[I");
    BOOST_REQUIRE_EQUAL(in.size(), 1u);
    BOOST_CHECK(in[0].kind == Event::Kind::focus);
    BOOST_CHECK(in[0].focus_gained);

    const auto out = decode_all("\e[O");
    BOOST_REQUIRE_EQUAL(out.size(), 1u);
    BOOST_CHECK(out[0].kind == Event::Kind::focus);
    BOOST_CHECK(!out[0].focus_gained);
}

BOOST_AUTO_TEST_CASE(chunk_invariance_on_mixed_corpus) {
    const std::string corpus =
        "typ打字ing\x7F\e[A\e[1;5C\e[200~pa\r\nste\e[201~\e[<0;3;4M\e[I"
        "\x01\eOS\e[?1;2c\e[24~\t";
    BOOST_CHECK(same_text_stream(decode_all(corpus), decode_bytewise(corpus)));
    std::mt19937 rng(7);
    for (int trial = 0; trial < 32; ++trial) {
        BOOST_CHECK(
            same_text_stream(decode_all(corpus), decode_split(corpus, rng)));
    }
}

BOOST_AUTO_TEST_CASE(fuzz_chunk_invariance) {
    // 对随机噪声同样要求"整块喂入 == 随机分块喂入"（§四）：
    // 抓"alt + 非法 UTF-8 多跳字节"这类只在不同切分下暴露的偏差。
    std::mt19937 rng(2024);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (int trial = 0; trial < 64; ++trial) {
        std::string noise;
        const int n = std::uniform_int_distribution<int>(0, 120)(rng);
        for (int i = 0; i < n; ++i) {
            noise += static_cast<char>(byte_dist(rng));
        }
        BOOST_CHECK(same_text_stream(decode_all(noise),
                                     decode_split(noise, rng)));
    }
}

BOOST_AUTO_TEST_CASE(router_reentrant_stack_mutation) {
    // 处理器在 on_event 里 push/pop 本路由（确认框弹自己、开新模态）
    // 不允许打乱下沉（bug 1）：路由沿快照进行，不崩溃、顺序正确。
    EventRouter router;
    Recorder newcomer;
    newcomer.consume = true;

    struct Pusher : EventHandler {
        EventRouter* router = nullptr;
        EventHandler* to_push = nullptr;
        int calls = 0;
        bool on_event(const Event&) override {
            ++calls;
            router->push(*to_push);
            return false;
        }
    } pusher;
    pusher.router = &router;
    pusher.to_push = &newcomer;

    struct SelfPop : EventHandler {
        EventRouter* router = nullptr;
        int calls = 0;
        bool on_event(const Event&) override {
            ++calls;
            router->pop(*this); // 弹出自己但不消费
            return false;
        }
    };

    router.push(pusher);
    const Event e = text_ev("x");
    BOOST_CHECK(!router.route(e));
    BOOST_REQUIRE_EQUAL(pusher.calls, 1);
    BOOST_CHECK(newcomer.hits.empty()); // 期间压栈的处理器不参与本次下发

    BOOST_CHECK(router.route(e)); // 下一次：newcomer 已在栈顶并消费
    BOOST_REQUIRE_EQUAL(pusher.calls, 1); // newcomer 在栈顶先收，消费即止
    BOOST_REQUIRE_EQUAL(newcomer.hits.size(), 1u);

    // 64 个"弹出自己"的处理器：下发期间持续修改栈，不得失效。
    std::vector<SelfPop> many(64);
    for (auto& s : many) {
        s.router = &router;
        router.push(s);
    }
    router.pop(pusher);
    router.pop(newcomer);
    BOOST_CHECK(!router.route(e));
    for (const auto& s : many) BOOST_CHECK_EQUAL(s.calls, 1);
    BOOST_CHECK(!router.route(e)); // 栈已空，不再有人被问到
    for (const auto& s : many) BOOST_CHECK_EQUAL(s.calls, 1);
}

BOOST_AUTO_TEST_CASE(router_pop_during_route_stops_pending_handlers) {
    // 复查 A：上层处理器在下发中弹出（并可能销毁）下层处理器 ——
    // 被弹出的下层本次还没轮到，也必须不再被调用。
    EventRouter router;
    Recorder lower, global;
    lower.consume = true;
    global.consume = true;
    router.set_global(&global);

    struct Closer : EventHandler {
        EventRouter* router = nullptr;
        EventHandler* below = nullptr;
        bool on_event(const Event&) override {
            router->pop(*below);
            router->pop(*this);
            return false;
        }
    } closer;
    closer.router = &router;
    closer.below = &lower;

    router.push(lower);
    router.push(closer);
    const Event e = text_ev("x");
    BOOST_CHECK(router.route(e));
    BOOST_CHECK(lower.hits.empty());          // 已弹出：不再收到本次事件
    BOOST_REQUIRE_EQUAL(global.hits.size(), 1u); // 下沉到全局兜底

    // 下发结束后空槽已压实：栈的后续 push/pop/route 行为正常。
    Recorder top;
    top.consume = true;
    router.push(top);
    BOOST_CHECK(router.route(e));
    BOOST_REQUIRE_EQUAL(top.hits.size(), 1u);
    router.pop(top);
    BOOST_CHECK(router.route(e));
    BOOST_REQUIRE_EQUAL(global.hits.size(), 2u);
    BOOST_CHECK(lower.hits.empty());

    // 嵌套下发：处理器在 on_event 里再次 route，内层结束不得提前压实
    // 外层还在按下标遍历的栈。
    struct Nested : EventHandler {
        EventRouter* router = nullptr;
        EventHandler* victim = nullptr;
        int depth = 0;
        bool on_event(const Event& ev) override {
            if (depth++ == 0) {
                router->pop(*victim);
                router->route(ev); // 内层下发
            }
            return false;
        }
    } nested;
    Recorder bottom, victim;
    bottom.consume = false;
    victim.consume = false;
    nested.router = &router;
    nested.victim = &victim;
    router.set_global(nullptr);
    router.push(bottom);
    router.push(victim);
    router.push(nested);
    BOOST_CHECK(!router.route(e));
    BOOST_CHECK(victim.hits.empty());
    BOOST_REQUIRE_EQUAL(bottom.hits.size(), 2u); // 内层一次 + 外层一次
    // 内层若提前压实，外层下标会错位到 nested 上再调它一次。
    BOOST_CHECK_EQUAL(nested.depth, 2);
}

BOOST_AUTO_TEST_CASE(fuzz_no_crash_no_swallow) {
    // 随机字节流（含截断转义序列、任意切分）不崩溃；喂一条完整序列
    // 让解码器脱离任何截断前缀后，完整的粘贴必须原样产出 ——
    // 解码器永远不卡死、不吞掉完整序列之后的输入。
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (int trial = 0; trial < 64; ++trial) {
        std::string noise;
        const int n = std::uniform_int_distribution<int>(0, 200)(rng);
        for (int i = 0; i < n; ++i) {
            noise += static_cast<char>(byte_dist(rng));
        }
        Decoder d;
        std::vector<Event> out;
        std::size_t pos = 0;
        while (pos < noise.size()) {
            std::size_t take =
                std::uniform_int_distribution<std::size_t>(1, 9)(rng);
            if (pos + take > noise.size()) take = noise.size() - pos;
            d.feed(std::string_view(noise).substr(pos, take), out);
            pos += take;
        }
        const std::size_t before = out.size();
        d.feed("\e[A\e[200~HELLO\e[201~", out);
        BOOST_REQUIRE_GT(out.size(), before);
        BOOST_CHECK_EQUAL(out.back().text, "HELLO");
        BOOST_CHECK(at(out, out.size() - 1).kind == Event::Kind::paste);
    }
}

BOOST_AUTO_TEST_CASE(router_sink_order) {
    // 固定下沉顺序：栈顶 → 焦点 → 全局；第一个 true 消费。
    EventRouter router;
    Recorder modal, focus, global;
    modal.consume = true;
    focus.consume = true;
    global.consume = true;
    router.push(modal);
    router.set_focus(&focus);
    router.set_global(&global);

    const Event e = text_ev("x");
    BOOST_CHECK(router.route(e));
    BOOST_REQUIRE_EQUAL(modal.hits.size(), 1u);
    BOOST_CHECK(focus.hits.empty());

    router.pop(modal);
    BOOST_CHECK(router.route(e));
    BOOST_REQUIRE_EQUAL(focus.hits.size(), 1u);

    router.set_focus(nullptr);
    BOOST_CHECK(router.route(e));
    BOOST_REQUIRE_EQUAL(global.hits.size(), 1u);

    // 栈内层叠：后压栈的（栈顶）先收。
    Recorder a, b;
    a.consume = false;
    b.consume = true;
    router.push(a);
    router.push(b);
    BOOST_CHECK(router.route(e));
    BOOST_REQUIRE_EQUAL(b.hits.size(), 1u); // b（栈顶）收下即止
    BOOST_CHECK(a.hits.empty());

    router.pop(b);
    BOOST_CHECK(router.route(e)); // a 不消费 → 全局兜底
    BOOST_REQUIRE_EQUAL(a.hits.size(), 1u);
    BOOST_REQUIRE_EQUAL(global.hits.size(), 2u);
}

BOOST_AUTO_TEST_CASE(input_box_handler_translation) {
    InputBox box;
    InputBoxHandler handler(box);

    const auto feed = [&](std::string_view s) {
        for (const auto& e : decode_all(s)) handler.on_event(e);
    };

    feed("ab");
    BOOST_CHECK_EQUAL(box.text(), "ab");
    handler.on_event(key_ev(Key::left));
    feed("X");
    BOOST_CHECK_EQUAL(box.text(), "aXb");
    handler.on_event(key_ev(Key::backspace));
    BOOST_CHECK_EQUAL(box.text(), "ab");

    // 编辑键消费、策略键不消费。
    BOOST_CHECK(handler.on_event(key_ev(Key::home)));
    BOOST_CHECK(handler.on_event(key_ev(Key::del)));
    BOOST_CHECK(handler.on_event(key_ev(Key::tab)));
    BOOST_CHECK(!handler.on_event(key_ev(Key::enter)));
    BOOST_CHECK(!handler.on_event(key_ev(Key::escape)));
    BOOST_CHECK(!handler.on_event(key_ev(Key::left, Mods::ctrl)));
    BOOST_CHECK(!handler.on_event(key_ev(Key::up, Mods::shift)));

    // 带修饰的 backspace 同样下沉给上层（词级删除是应用策略）。
    BOOST_CHECK(!handler.on_event(key_ev(Key::backspace, Mods::ctrl)));

    // 粘贴事件直接进输入框（bug 8）：\r\n 规范化，换行不触发提交。
    InputBox paste_box;
    InputBoxHandler paste_handler(paste_box);
    Event paste;
    paste.kind = Event::Kind::paste;
    paste.text = "x\r\ny";
    BOOST_CHECK(paste_handler.on_event(paste));
    BOOST_CHECK_EQUAL(paste_box.text(), "x\ny");
}

BOOST_AUTO_TEST_SUITE_END()
