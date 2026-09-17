#include <boost/test/unit_test.hpp>

#include <random>

#include "support.hpp"
#include "tui/surface.hpp"

using namespace dagent::tui;
using test_support::AllocScope;
using test_support::screen_mismatch;
using test_support::VtScreen;

namespace {

// 一帧：render_frame → 回放 → 交换（与 present() 的非 I/O 部分相同）。
void frame(std::string& out, Surface& back, Surface& front, VtScreen& vt,
           const FrameOptions& opt = {}) {
    render_frame(out, back, front, opt);
    vt.feed(out);
    std::swap(front, back);
    back.clear_dirty();
}

Style random_style(std::mt19937& rng) {
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    auto color = [&]() -> Color {
        switch (pick(0, 2)) {
        case 0: return Color{};
        case 1: return Color::indexed(static_cast<uint8_t>(pick(0, 255)));
        default:
            return Color::rgb(static_cast<uint8_t>(pick(0, 255)), static_cast<uint8_t>(pick(0, 255)),
                              static_cast<uint8_t>(pick(0, 255)));
        }
    };
    return Style{color(), color(), static_cast<Attr>(pick(0, 127))};
}

} // namespace

BOOST_AUTO_TEST_SUITE(present)

BOOST_AUTO_TEST_CASE(random_frames_replay_to_back_buffer) {
    const std::vector<std::string> glyphs = {"a", "Z", " ", "中", "e\U00000301", "\U0001F600",
                                             "\U0001F468\U0000200D\U0001F469\U0000200D\U0001F467"};
    std::mt19937 rng(7);
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    Surface front(23, 7), back(23, 7);
    VtScreen vt(23, 7);
    std::string out;
    for (int f = 0; f < 400; ++f) {
        back.copy_from(front);
        for (int k = pick(0, 12); k > 0; --k) {
            const Style st = random_style(rng);
            const std::string& g = glyphs[static_cast<std::size_t>(pick(0, 6))];
            switch (pick(0, 2)) {
            case 0: back.put(pick(0, 22), pick(0, 6), g, st); break;
            case 1: back.text(pick(-2, 22), pick(0, 6), g + g + "x", st); break;
            case 2: back.fill({pick(0, 22), pick(0, 6), pick(1, 6), pick(1, 3)}, U'中', st); break;
            }
        }
        frame(out, back, front, vt, {.synchronized = pick(0, 1) == 1, .truecolor = true});
        // 交换后 front 即本帧 back
        BOOST_REQUIRE_MESSAGE(vt.errors().empty(), "frame " << f << ": " << vt.errors().front());
        BOOST_REQUIRE_MESSAGE(screen_mismatch(vt, front).empty(),
                              "frame " << f << ": " << screen_mismatch(vt, front));
        BOOST_REQUIRE(vt.pen() == Style{});  // 帧末样式归零
        BOOST_REQUIRE(vt.sync_depth() == 0);
    }
}

BOOST_AUTO_TEST_CASE(unchanged_or_rewritten_identical_rows_produce_no_cell_output) {
    Surface front(10, 3), back(10, 3);
    VtScreen vt(10, 3);
    std::string out;
    back.text(0, 1, "hello", Style{});
    frame(out, back, front, vt);

    back.copy_from(front);
    render_frame(out, back, front, {});
    BOOST_TEST(out == "\x1b[?25l");  // 没有任何行变脏

    back.text(0, 1, "hello", Style{});  // 行被重画成相同内容
    render_frame(out, back, front, {});
    BOOST_TEST(out == "\x1b[?25l");
}

BOOST_AUTO_TEST_CASE(single_cell_change_emits_only_that_span) {
    Surface front(200, 50), back(200, 50);
    VtScreen vt(200, 50);
    std::string out;
    const Style st{Color::rgb(10, 20, 30), Color::indexed(4), Attr::bold};
    for (int r = 0; r < 50; ++r) back.fill({0, r, 200, 1}, U'x', st);
    frame(out, back, front, vt, {.truecolor = true});
    const std::size_t full = out.size();

    back.copy_from(front);
    back.put(120, 33, "y", st);
    frame(out, back, front, vt, {.truecolor = true});
    BOOST_TEST(out == "\x1b[?25l\x1b[34;121H\x1b[1m\x1b[38;2;10;20;30m\x1b[48;5;4my\x1b[0m");
    BOOST_TEST(full > 100 * out.size());
    BOOST_TEST(screen_mismatch(vt, front) == "");
}

BOOST_AUTO_TEST_CASE(sgr_delta_resends_surviving_bold_or_dim) {
    Surface front(2, 1), back(2, 1);
    VtScreen vt(2, 1);
    std::string out;
    back.put(0, 0, "a", Style{Color{}, Color{}, Attr::bold | Attr::dim});
    back.put(1, 0, "b", Style{Color{}, Color{}, Attr::dim});
    frame(out, back, front, vt);
    BOOST_TEST(out == "\x1b[?25l\x1b[1;1H\x1b[1m\x1b[2ma\x1b[22m\x1b[2mb\x1b[0m");
    BOOST_TEST(screen_mismatch(vt, front) == "");
}

BOOST_AUTO_TEST_CASE(rgb_is_quantized_without_truecolor) {
    Surface front(2, 1), back(2, 1);
    std::string out;
    back.put(0, 0, "a", Style{Color::rgb(255, 0, 0), Color::rgb(128, 128, 128), Attr::none});
    render_frame(out, back, front, {.truecolor = false});
    BOOST_TEST(out == "\x1b[?25l\x1b[1;1H\x1b[38;5;196m\x1b[48;5;244ma\x1b[0m");
}

BOOST_AUTO_TEST_CASE(cursor_and_synchronized_output_wrap_the_frame) {
    Surface front(4, 2), back(4, 2);
    std::string out;
    back.put(0, 0, "a", Style{});
    render_frame(out, back, front, {.synchronized = true, .cursor = Point{2, 1}});
    BOOST_TEST(out == "\x1b[?2026h\x1b[?25l\x1b[1;1Ha\x1b[2;3H\x1b[?25h\x1b[?2026l");
}

BOOST_AUTO_TEST_CASE(size_mismatch_repaints_every_cell_over_stale_screen) {
    Surface front(10, 4), back(12, 5);
    back.text(0, 2, "resized", Style{});
    VtScreen vt(10, 4);
    vt.scribble();   // 终端上是旧帧内容
    vt.resize(12, 5);
    std::string out;
    frame(out, back, front, vt);
    BOOST_TEST(vt.errors().empty());
    BOOST_TEST(screen_mismatch(vt, front) == "");
}

BOOST_AUTO_TEST_CASE(full_repaint_ignores_row_dirty_flags) {
    Surface front(6, 2), back(6, 2);
    back.copy_from(front);  // 行脏标记全清
    VtScreen vt(6, 2);
    vt.scribble();
    std::string out;
    frame(out, back, front, vt, {.full_repaint = true});
    BOOST_TEST(screen_mismatch(vt, front) == "");
}

BOOST_AUTO_TEST_CASE(steady_state_frame_does_not_allocate) {
    Surface front(120, 40), back(120, 40);
    VtScreen vt(120, 40);
    std::string out;
    const Style st{Color::rgb(1, 2, 3), Color{}, Attr::underline};
    for (int warm = 0; warm < 3; ++warm) {
        back.copy_from(front);
        back.text(0, warm, "预热 warm-up frame", st);
        frame(out, back, front, vt, {.truecolor = true, .cursor = Point{3, 3}});
    }

    long allocations = -1;
    {
        AllocScope scope;
        back.copy_from(front);
        back.text(5, 7, "流式 token", st);
        back.put(0, 39, "⠙", Style{});
        render_frame(out, back, front, {.truecolor = true, .cursor = Point{3, 3}});
        std::swap(front, back);
        back.clear_dirty();
        allocations = scope.allocations();
    }
    BOOST_TEST(allocations == 0);
}

BOOST_AUTO_TEST_SUITE_END()
