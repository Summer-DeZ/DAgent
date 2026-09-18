// L3 层栈验收（02-tui-final-update §3.4）：摆放与夹取、命中、移动与摘除。
// 几何是纯计算，直接用真实控件树断言，不做模拟。

#include <boost/test/unit_test.hpp>

#include <memory>
#include <ostream>

#include "tui/layout.hpp"
#include "tui/widget.hpp"

using namespace dagent::tui;

namespace dagent::tui {
inline std::ostream& operator<<(std::ostream& os, const Rect& r) {
    return os << "Rect{" << r.x << "," << r.y << "," << r.w << "," << r.h << "}";
}
} // namespace dagent::tui

namespace {

// 固定尺寸方块：measure 返回指定尺寸，render 整块填成给定字符。
class Block : public Widget {
public:
    Block(int w, int h, char32_t ch) : w_(w), h_(h), ch_(ch) {}
    Size measure(Size) const override { return {w_, h_}; }
    void render(Surface& s) override {
        s.fill({0, 0, s.cols(), s.rows()}, ch_, Style{});
    }

private:
    int w_;
    int h_;
    char32_t ch_;
};

} // namespace

BOOST_AUTO_TEST_SUITE(layout)

BOOST_AUTO_TEST_CASE(placements_are_measured_and_screen_clamped) {
    LayerStack stack{std::make_unique<Container>(Container::Direction::vertical)};
    stack.layout({0, 0, 40, 10});

    // center：40×10 屏幕上的 10×4 → (15,3)。
    auto center = std::make_unique<Block>(10, 4, U'C');
    Block* c = center.get();
    const uint32_t center_id = stack.push(std::move(center), Placement::center);
    BOOST_TEST(c->rect() == (Rect{15, 3, 10, 4}));

    // top_right：贴右上角。
    auto toast = std::make_unique<Block>(10, 4, U'T');
    Block* t = toast.get();
    stack.push(std::move(toast), Placement::top_right);
    BOOST_TEST(t->rect() == (Rect{30, 0, 10, 4}));

    // above_point：点 (5,8) 上方有 8 行 → y = 8-4 = 4。
    auto above = std::make_unique<Block>(10, 4, U'A');
    Block* a = above.get();
    stack.push(std::move(above), Placement::above_point, {5, 8});
    BOOST_TEST(a->rect() == (Rect{5, 4, 10, 4}));

    // 上方空间不足（点 y=2 < 高度 4）→ 翻到点下方。
    stack.move(center_id, Placement::above_point, {5, 2});
    BOOST_TEST(c->rect() == (Rect{5, 2, 10, 4}));

    // 贴右缘：x 被夹回屏幕内。
    stack.move(center_id, Placement::above_point, {35, 8});
    BOOST_TEST(c->rect() == (Rect{30, 4, 10, 4}));

    // at_point 靠近右下角：整体夹进屏幕。
    stack.move(center_id, Placement::at_point, {100, 100});
    BOOST_TEST(c->rect() == (Rect{30, 6, 10, 4}));

    // 超过屏幕的浮层：尺寸先被夹到屏幕大小。
    auto huge = std::make_unique<Block>(100, 50, U'H');
    Block* h = huge.get();
    stack.push(std::move(huge), Placement::center);
    BOOST_TEST(h->rect() == (Rect{0, 0, 40, 10}));
}

BOOST_AUTO_TEST_CASE(hit_finds_topmost_deepest_widget) {
    auto base = std::make_unique<Container>(Container::Direction::vertical);
    auto text = std::make_unique<Text>();
    text->set_text("base");
    Text* base_text = text.get();
    base->add({Sizing::fixed, 5}, std::move(text));
    Widget* base_p = base.get();
    LayerStack stack{std::move(base)};
    stack.layout({0, 0, 40, 10});

    // 无浮层：命中基础层最深控件；容器自身只在空隙里兜底。
    BOOST_TEST(stack.hit({1, 1}) == static_cast<Widget*>(base_text));
    BOOST_TEST(stack.hit({39, 9}) == base_p);

    // 浮层内的子控件优先于基础层，且后 push 的压在上面。
    auto dialog = std::make_unique<Container>(Container::Direction::vertical);
    auto label = std::make_unique<Text>();
    label->set_text("dialog");
    Text* label_p = label.get();
    // content：让对话框的自然宽度取文本宽度（fixed 子项不计副轴尺寸）。
    dialog->add({Sizing::content, 0, 1, 2}, std::move(label));
    const uint32_t id = stack.push(std::move(dialog), Placement::at_point, {10, 2});
    BOOST_TEST(stack.hit({11, 2}) == static_cast<Widget*>(label_p));
    BOOST_TEST(stack.hit({1, 1}) == static_cast<Widget*>(base_text)); // 浮层外不变

    auto top = std::make_unique<Block>(8, 3, U'X');
    Block* top_p = top.get();
    const uint32_t top_id = stack.push(std::move(top), Placement::at_point, {10, 2});
    BOOST_TEST(stack.hit({11, 3}) == static_cast<Widget*>(top_p));

    // 摘除后浮层不再参与命中。
    std::unique_ptr<Widget> removed = stack.remove(id);
    BOOST_TEST(removed != nullptr);
    BOOST_TEST(stack.hit({11, 3}) == static_cast<Widget*>(top_p));
    stack.remove(top_id);
    BOOST_TEST(stack.hit({11, 3}) == static_cast<Widget*>(base_text));
    BOOST_TEST(stack.remove(999) == nullptr); // 不存在
}

BOOST_AUTO_TEST_CASE(move_repositions_and_remove_returns_ownership) {
    LayerStack stack{std::make_unique<Container>(Container::Direction::vertical)};
    stack.layout({0, 0, 20, 6});

    auto block = std::make_unique<Block>(4, 2, U'B');
    Block* p = block.get();
    const uint32_t id = stack.push(std::move(block), Placement::at_point, {0, 0});
    BOOST_TEST(p->rect() == (Rect{0, 0, 4, 2}));

    stack.move(id, Placement::at_point, {16, 4});
    BOOST_TEST(p->rect() == (Rect{16, 4, 4, 2}));

    stack.move(id, Placement::top_right);
    BOOST_TEST(p->rect() == (Rect{16, 0, 4, 2}));

    std::unique_ptr<Widget> owned = stack.remove(id);
    BOOST_TEST(owned.get() == p);
    BOOST_TEST(stack.hit({16, 1}) != static_cast<Widget*>(p)); // 已不在树上
    BOOST_TEST(stack.hit({16, 1}) != nullptr);                // 落回基础层
}

BOOST_AUTO_TEST_SUITE_END()
