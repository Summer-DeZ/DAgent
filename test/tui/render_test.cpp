// 渲染层（§4.2–§4.4）：出帧差分、字素与宽度、布局、浮层。
// 直接驱动真实的 Surface / render_frame / Container / LayerStack；
// 字素一致性用 Unicode 官方 GraphemeBreakTest.txt（test/tui/data/）。

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/grapheme.hpp"
#include "tui/layout.hpp"
#include "tui/surface.hpp"
#include "tui/widget.hpp"

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


// 虚拟终端：把 render_frame 的输出字节重放到一块网格上（CUP + SGR + 文本）。
struct VirtualScreen {
    Surface cells;
    int x = 0;
    int y = 0;
    Style style{};

    VirtualScreen(int cols, int rows) : cells(cols, rows) {}

    void sgr(std::string_view params) {
        std::vector<int> v;
        std::size_t i = 0;
        while (i <= params.size()) {
            const std::size_t j = params.find(';', i);
            const std::string_view p =
                params.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i);
            v.push_back(p.empty() ? 0 : std::stoi(std::string(p)));
            if (j == std::string_view::npos) break;
            i = j + 1;
        }
        for (std::size_t k = 0; k < v.size(); ++k) {
            const int c = v[k];
            const auto color = [&](Color& dst) {
                if (v[k + 1] == 5) {
                    dst = Color::indexed(static_cast<uint8_t>(v[k + 2]));
                    k += 2;
                } else {
                    dst = Color::rgb(static_cast<uint8_t>(v[k + 2]),
                                     static_cast<uint8_t>(v[k + 3]),
                                     static_cast<uint8_t>(v[k + 4]));
                    k += 4;
                }
            };
            switch (c) {
            case 0: style = Style{}; break;
            case 1: style.attrs = style.attrs | Attr::bold; break;
            case 2: style.attrs = style.attrs | Attr::dim; break;
            case 22: style.attrs = style.attrs & ~(Attr::bold | Attr::dim); break;
            case 3: style.attrs = style.attrs | Attr::italic; break;
            case 23: style.attrs = style.attrs & ~Attr::italic; break;
            case 4: style.attrs = style.attrs | Attr::underline; break;
            case 24: style.attrs = style.attrs & ~Attr::underline; break;
            case 7: style.attrs = style.attrs | Attr::reverse; break;
            case 27: style.attrs = style.attrs & ~Attr::reverse; break;
            case 38: color(style.fg); break;
            case 39: style.fg = Color{}; break;
            case 48: color(style.bg); break;
            case 49: style.bg = Color{}; break;
            default: break;
            }
        }
    }

    void apply(std::string_view s) {
        while (!s.empty()) {
            if (s[0] == '\x1b' && s.size() > 1 && s[1] == '[') {
                std::size_t i = 2;
                while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
                const std::string_view params = s.substr(2, i - 2);
                const char final_byte = s[i];
                s.remove_prefix(i + 1);
                if (final_byte == 'H') {
                    const std::size_t semi = params.find(';');
                    y = std::stoi(std::string(params.substr(0, semi))) - 1;
                    x = std::stoi(std::string(params.substr(semi + 1))) - 1;
                } else if (final_byte == 'm') {
                    sgr(params);
                }
                continue; // 私有模式（?25l、?2026h）不影响网格
            }
            unicode::Grapheme g;
            if (!unicode::next_grapheme(s, g)) break;
            cells.put(x, y, g.bytes, style);
            x += g.width;
        }
    }
};

// 逐格比较；返回第一处差异的描述，相同时返回空串。
std::string first_difference(const Surface& a, const Surface& b) {
    for (int r = 0; r < a.rows(); ++r) {
        for (int c = 0; c < a.cols(); ++c) {
            if (!(a.at(c, r) == b.at(c, r))) {
                return "cell (" + std::to_string(c) + "," + std::to_string(r) +
                       ") '" + std::string(a.at(c, r).grapheme()) + "' vs '" +
                       std::string(b.at(c, r).grapheme()) + "'";
            }
        }
    }
    return {};
}

// 与 Runtime 相同的帧序：布局（按需）→ 渲染到上一帧之上。
void frame(Widget& root, Surface& s) {
    const Rect area{0, 0, s.cols(), s.rows()};
    if (root.needs_layout() || !(root.rect() == area)) root.layout(area);
    root.render(s);
}

// 同一棵树整树失效后渲染到空白网格：增量结果必须与之逐格相同。
std::string full_render_difference(Widget& root, const Surface& incremental) {
    Surface full(incremental.cols(), incremental.rows());
    root.invalidate_tree();
    root.render(full);
    return first_difference(incremental, full);
}

std::unique_ptr<Text> text_widget(std::string s) {
    auto t = std::make_unique<Text>();
    t->set_text(std::move(s));
    return t;
}

} // namespace

BOOST_AUTO_TEST_SUITE(render)

// 出帧只写变化的单元格；把输出重放到虚拟终端上，结果与本帧网格逐格一致。
BOOST_AUTO_TEST_CASE(frame_diff_rewrites_only_changed_cells) {
    constexpr int cols = 20;
    constexpr int rows = 4;
    const Style warm{Color::rgb(200, 100, 50), Color{}, Attr::bold};
    Surface back(cols, rows);
    Surface front(cols, rows);
    VirtualScreen screen(cols, rows);
    std::string out;

    // 与 present() 相同的双缓冲交换。
    const auto swap_buffers = [&] {
        std::swap(front, back);
        back.clear_dirty();
        back.copy_from(front);
    };

    // 第一帧：宽字符与带样式文本，光标定位后显示。
    back.text(0, 0, "hello 世界", Style{});
    back.text(0, 1, "status: idle", warm);
    render_frame(out, back, front, {true, true, Point{3, 2}, false});
    BOOST_TEST(out.starts_with("\x1b[?2026h\x1b[?25l"));
    BOOST_TEST(out.ends_with("\x1b[3;4H\x1b[?25h\x1b[?2026l"));
    screen.apply(out);
    BOOST_TEST(first_difference(screen.cells, back) == "");
    swap_buffers();

    // 第二帧：只改 "idle" → "busy"，输出只有这 4 格。
    back.text(8, 1, "busy", warm);
    render_frame(out, back, front, {});
    BOOST_TEST(out == "\x1b[?25l\x1b[2;9H\x1b[1m\x1b[38;2;200;100;50mbusy\x1b[0m");
    screen.apply(out);
    BOOST_TEST(first_difference(screen.cells, back) == "");
    swap_buffers();

    // 第三帧：覆盖宽字符「世」的右半，左半随之失效，重放后仍一致。
    back.put(7, 0, "X", Style{});
    render_frame(out, back, front, {});
    screen.apply(out);
    BOOST_TEST(first_difference(screen.cells, back) == "");
    BOOST_TEST(back.at(6, 0).grapheme() == " ");
    swap_buffers();

    // 没有变化：不写任何单元格。
    render_frame(out, back, front, {});
    BOOST_TEST(out == "\x1b[?25l");

    // 非真彩色终端：RGB 换算成 256 色。
    Surface blank(cols, rows);
    render_frame(out, front, blank, {false, false, std::nullopt, true});
    BOOST_TEST(out.find("\x1b[38;2;") == std::string::npos);
    BOOST_TEST(out.find("\x1b[38;5;167m") != std::string::npos);
}

// 字素切分与 Unicode 官方测试一致（按清单排除 GB9b/GB9c），宽度符合终端显示。
BOOST_AUTO_TEST_CASE(grapheme_clusters_and_widths_follow_unicode) {
    std::ifstream in(std::string(DAGENT_TEST_DATA_DIR) + "/GraphemeBreakTest.txt");
    BOOST_REQUIRE(in);
    std::size_t checked = 0;
    std::size_t excluded = 0;
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
            if (tok == "\xC3\xB7") { // ÷
                break_before = true;
            } else if (tok == "\xC3\x97") { // ×
                break_before = false;
            } else {
                cps.push_back(static_cast<char32_t>(std::stoul(tok, nullptr, 16)));
                expect.push_back(break_before);
            }
        }
        if (cps.empty()) continue;
        if (excluded_case(cps)) {
            ++excluded;
            continue;
        }
        ++checked;
        std::string text;
        for (char32_t cp : cps) text += encode(cp);
        std::string_view sv = text;
        std::vector<bool> got;
        unicode::Grapheme g;
        while (unicode::next_grapheme(sv, g)) {
            std::string_view cluster = g.bytes;
            bool first = true;
            while (!cluster.empty()) {
                unicode::decode_utf8(cluster);
                got.push_back(first);
                first = false;
            }
        }
        if (got != expect && failures.size() < 5) failures.push_back(describe(cps));
    }
    BOOST_TEST(checked > 600);
    BOOST_TEST(excluded > 50);
    BOOST_TEST(failures.empty(), "mismatch: " << (failures.empty() ? "" : failures[0]));

    const auto cluster_width = [](std::string_view s) {
        unicode::Grapheme g;
        BOOST_REQUIRE(unicode::next_grapheme(s, g));
        BOOST_REQUIRE(s.empty()); // 整串是一个簇
        return g.width;
    };
    BOOST_TEST(cluster_width("a") == 1);
    BOOST_TEST(cluster_width("中") == 2);
    BOOST_TEST(cluster_width("e\xCC\x81") == 1);                          // e + 组合重音
    BOOST_TEST(cluster_width("\xE2\x9D\xA4") == 1);                        // ❤ 文本呈现
    BOOST_TEST(cluster_width("\xE2\x9D\xA4\xEF\xB8\x8F") == 2);            // ❤️ VS16
    BOOST_TEST(cluster_width("\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3") == 2);    // 🇨🇳
    BOOST_TEST(cluster_width("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
                             "\xE2\x80\x8D\xF0\x9F\x91\xA7") == 2);        // 👨‍👩‍👧

    // 网格里宽字符占两格，超长字素（ZWJ 序列）照样还原。
    Surface s(10, 1);
    BOOST_TEST(s.text(0, 0, "中\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9x", Style{}) == 5);
    BOOST_TEST(s.at(1, 0).width == 0);
    BOOST_TEST(s.at(2, 0).grapheme() == "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9");
    BOOST_TEST(s.at(4, 0).grapheme() == "x");
}

// fixed / content / flex 的分配、内容变化后的重新布局、空间不足时的压缩、嵌套坐标。
BOOST_AUTO_TEST_CASE(container_distributes_fixed_content_flex) {
    Container root{Container::Direction::vertical};
    auto header = text_widget("header");
    auto body = text_widget("a\nb\nc");
    auto notice = std::make_unique<Notice>();
    auto upper = text_widget("upper");
    auto row = std::make_unique<Container>(Container::Direction::horizontal);
    Widget* const header_p = header.get();
    Widget* const body_p = body.get();
    Notice* const notice_p = notice.get();
    Widget* const upper_p = upper.get();
    Widget* const row_p = row.get();
    auto label = text_widget("label");
    auto rest = text_widget("rest");
    Widget* const label_p = label.get();
    Widget* const rest_p = rest.get();
    row->add({Sizing::fixed, 6}, std::move(label));
    row->add({Sizing::flex, 1}, std::move(rest));

    root.add({Sizing::fixed, 2}, std::move(header));
    root.add({Sizing::content, 0, 1}, std::move(body));
    root.add({Sizing::content}, std::move(notice));
    root.add({Sizing::flex, 1}, std::move(upper));
    root.add({Sizing::flex, 2}, std::move(row));

    Surface s(20, 10);
    frame(root, s);
    // 剩余 5 行按 1:2 分：floor 得 1 与 3，余下 1 行补给先声明的。
    BOOST_TEST((header_p->rect() == Rect{0, 0, 20, 2}));
    BOOST_TEST((body_p->rect() == Rect{0, 2, 20, 3}));
    BOOST_TEST((notice_p->rect() == Rect{0, 5, 20, 0})); // 空通知不占位
    BOOST_TEST((upper_p->rect() == Rect{0, 5, 20, 2}));
    BOOST_TEST((row_p->rect() == Rect{0, 7, 20, 3}));
    // 嵌套容器：子项坐标相对父容器，屏幕坐标沿父链累加。
    BOOST_TEST((label_p->rect() == Rect{0, 0, 6, 3}));
    BOOST_TEST((rest_p->screen_rect() == Rect{6, 7, 14, 3}));
    BOOST_TEST(s.at(0, 7).grapheme() == "l");
    BOOST_TEST(s.at(6, 7).grapheme() == "r");
    BOOST_TEST(root.hit_test({8, 8}) == rest_p);

    // 通知出现：需要重新布局，flex 让出一行。
    notice_p->show(Notice::Severity::warn, "careful");
    BOOST_TEST(root.needs_layout());
    frame(root, s);
    BOOST_TEST((notice_p->rect() == Rect{0, 5, 20, 1}));
    BOOST_TEST((upper_p->rect() == Rect{0, 6, 20, 2}));
    BOOST_TEST((row_p->rect() == Rect{0, 8, 20, 2}));
    BOOST_TEST(s.at(0, 5).grapheme() == "c");

    // 只有 4 行：从最后声明的开始压到 min，body 保住 min 1 之上的 2 行。
    root.layout({0, 0, 20, 4});
    BOOST_TEST((header_p->rect() == Rect{0, 0, 20, 2}));
    BOOST_TEST((body_p->rect() == Rect{0, 2, 20, 2}));
    BOOST_TEST(notice_p->rect().h == 0);
    BOOST_TEST(upper_p->rect().h == 0);
    BOOST_TEST(row_p->rect().h == 0);
}

// 浮层的摆放、命中，以及打开、移动、关闭、下层变化之后，增量渲染与全量渲染逐格相同。
BOOST_AUTO_TEST_CASE(overlays_place_hit_and_repaint_like_full_render) {
    auto base = std::make_unique<Container>(Container::Direction::vertical);
    auto top = text_widget("top-0\ntop-1\ntop-2\ntop-3\ntop-4");
    auto bottom = text_widget("bottom-5\nbottom-6\nbottom-7\nbottom-8\nbottom-9");
    Text* const top_p = top.get();
    Text* const bottom_p = bottom.get();
    base->add({Sizing::fixed, 5}, std::move(top));
    base->add({Sizing::fixed, 5}, std::move(bottom));
    LayerStack root{std::move(base)};

    Surface s(30, 10);
    frame(root, s);

    auto dialog = text_widget("abc\ndef");
    dialog->set_style({Color{}, Color{}, Attr::reverse});
    Widget* const dialog_p = dialog.get();
    const uint32_t dialog_id = root.push(std::move(dialog), Placement::center);
    frame(root, s);
    BOOST_TEST((dialog_p->screen_rect() == Rect{13, 4, 3, 2})); // 居中
    BOOST_TEST(s.at(13, 4).grapheme() == "a");
    BOOST_TEST(full_render_difference(root, s) == "");

    root.move(dialog_id, Placement::top_right);
    frame(root, s);
    BOOST_TEST((dialog_p->screen_rect() == Rect{27, 0, 3, 2}));
    BOOST_TEST(s.at(13, 4).grapheme() == " "); // 旧位置露出下层（"top-4" 之后是空白）
    BOOST_TEST(full_render_difference(root, s) == "");

    // 贴点上方放不下：翻到点的下方。
    auto popup = text_widget("xyz");
    Widget* const popup_p = popup.get();
    const uint32_t popup_id = root.push(std::move(popup), Placement::above_point, {5, 0});
    frame(root, s);
    BOOST_TEST((popup_p->screen_rect() == Rect{5, 0, 3, 1}));
    BOOST_TEST(full_render_difference(root, s) == "");

    // 命中取最上层：浮层优先，其次是基础层里最深的控件。
    BOOST_TEST(root.hit({28, 1}) == dialog_p);
    BOOST_TEST(root.hit({6, 0}) == popup_p);
    BOOST_TEST(root.hit({0, 9}) == bottom_p);
    BOOST_TEST(root.hit({0, 2}) == top_p);

    // 下层内容变化，浮层照样盖在上面。
    top_p->set_text("changed-0\nchanged-1\nchanged-2\nchanged-3\nchanged-4");
    frame(root, s);
    BOOST_TEST(s.at(5, 0).grapheme() == "x");
    BOOST_TEST(full_render_difference(root, s) == "");

    BOOST_TEST(root.remove(dialog_id) != nullptr);
    frame(root, s);
    BOOST_TEST(full_render_difference(root, s) == "");
    BOOST_TEST(root.remove(popup_id) != nullptr);
    frame(root, s);
    BOOST_TEST(full_render_difference(root, s) == "");
    BOOST_TEST(s.at(5, 0).grapheme() == "e"); // "changed-0"
    BOOST_TEST(root.remove(popup_id) == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
