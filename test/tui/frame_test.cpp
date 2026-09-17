// L2–L4 组合：控件树 → Surface → 差分字节 → VT 回放，逐帧断言屏幕与输出量。
#include <boost/test/unit_test.hpp>

#include "support.hpp"
#include "tui/widget.hpp"

using namespace dagent::tui;
using test_support::AllocScope;
using test_support::screen_mismatch;
using test_support::VtScreen;

namespace {

class ChatScreen {
public:
    ChatScreen(int cols, int rows) : cols_(cols), rows_(rows), vt(cols, rows) {
        log = add<Text>({Sizing::flex, 1});
        activity = add<Activity>({Sizing::content, 0, 0, 1});
        notice = add<Notice>({Sizing::content, 0, 0, 1});
        input = add<InputBox>({Sizing::content, 0, 3, 6});
    }

    void resize(int cols, int rows) {
        cols_ = cols;
        rows_ = rows;
        vt.resize(cols, rows);  // 终端改尺寸：重叠区域仍是旧内容
    }

    // 一帧中不做 I/O 的部分（稳态零分配的断言范围）。
    void compose() {
        back_.resize(cols_, rows_);
        back_.copy_from(front_);
        const Rect area{0, 0, cols_, rows_};
        if (root_.needs_layout() || !(root_.rect() == area)) root_.layout(area);
        root_.render(back_);
        std::optional<Point> cursor;
        if (auto c = input->cursor()) {
            cursor = Point{input->rect().x + c->x, input->rect().y + c->y};
        }
        render_frame(out, back_, front_, {.cursor = cursor});
        std::swap(front_, back_);
        back_.clear_dirty();
    }

    void frame() {
        compose();
        vt.feed(out);
    }

    std::vector<int> touched_rows() const { return test_support::cup_rows(out); }

    const Surface& screen() const { return front_; }

    Text* log = nullptr;
    Activity* activity = nullptr;
    Notice* notice = nullptr;
    InputBox* input = nullptr;
    std::string out;

private:
    template <class W>
    W* add(Constraint c) {
        auto w = std::make_unique<W>();
        W* raw = w.get();
        root_.add(c, std::move(w));
        return raw;
    }

    int cols_;
    int rows_;
    Container root_;
    Surface front_, back_;

public:
    VtScreen vt;
};

void require_screen_matches(const ChatScreen& ui) {
    BOOST_REQUIRE_MESSAGE(ui.vt.errors().empty(), ui.vt.errors().front());
    BOOST_REQUIRE_MESSAGE(screen_mismatch(ui.vt, ui.screen()).empty(),
                          screen_mismatch(ui.vt, ui.screen()));
}

} // namespace

BOOST_AUTO_TEST_SUITE(frame)

BOOST_AUTO_TEST_CASE(first_frame_paints_and_idle_frame_only_places_cursor) {
    ChatScreen ui(40, 12);
    ui.log->set_text("第一行\n第二行\twith tab");
    ui.frame();
    require_screen_matches(ui);
    BOOST_TEST(test_support::row_text(ui.screen(), 9) == "┌──────────────────────────────────────┐");

    ui.frame();
    BOOST_TEST(ui.out == "\x1b[?25l\x1b[11;2H\x1b[?25h");
}

BOOST_AUTO_TEST_CASE(spinner_tick_rewrites_one_cell_without_allocating) {
    ChatScreen ui(40, 12);
    ui.activity->set_action("思考中");
    ui.frame();
    ui.activity->tick();
    ui.frame();  // 预热：out 容量到位
    require_screen_matches(ui);

    long allocations = -1;
    {
        AllocScope scope;
        ui.activity->tick();
        ui.compose();
        allocations = scope.allocations();
    }
    ui.vt.feed(ui.out);
    require_screen_matches(ui);
    BOOST_TEST(allocations == 0);
    BOOST_TEST(ui.out == "\x1b[?25l\x1b[9;1H⠹\x1b[11;2H\x1b[?25h");
}

BOOST_AUTO_TEST_CASE(typing_touches_only_the_input_row) {
    ChatScreen ui(40, 12);
    ui.log->set_text("history");
    ui.frame();
    for (std::string_view g : {"h", "é", "l", "l", "o", " ", "世", "界", "\t", "!"}) {
        ui.input->insert(g);
        ui.frame();
        require_screen_matches(ui);
        for (int row : ui.touched_rows()) BOOST_TEST(row == 11);
    }
}

BOOST_AUTO_TEST_CASE(layout_epochs_keep_screen_consistent) {
    ChatScreen ui(30, 10);
    ui.log->set_text("a\nb\nc\nd\ne\nf\ng\nh");
    ui.frame();

    ui.notice->show(Notice::Severity::warn, "磁盘将满");
    ui.activity->set_action("写入");
    ui.frame();
    require_screen_matches(ui);

    ui.input->insert("第一行\n第二行\n第三行");  // 输入框长高，滚动区让出空间
    ui.frame();
    require_screen_matches(ui);

    ui.notice->show(Notice::Severity::info, "");
    ui.activity->set_action("");
    ui.input->set_text("");
    ui.frame();
    require_screen_matches(ui);
    BOOST_TEST(test_support::row_text(ui.screen(), 6) == std::string("g") + std::string(29, ' '));
}

BOOST_AUTO_TEST_CASE(terminal_resize_repaints_over_stale_content) {
    ChatScreen ui(40, 12);
    ui.log->set_text("before resize");
    ui.input->insert("draft");
    ui.frame();

    ui.resize(30, 8);
    ui.frame();
    require_screen_matches(ui);

    ui.resize(50, 14);
    ui.vt.scribble();  // 最坏情况：终端上全是垃圾
    ui.frame();
    require_screen_matches(ui);

    ui.input->insert("!");  // 尺寸稳定后回到增量路径
    ui.frame();
    require_screen_matches(ui);
    for (int row : ui.touched_rows()) BOOST_TEST(row == 13);
}

BOOST_AUTO_TEST_SUITE_END()
