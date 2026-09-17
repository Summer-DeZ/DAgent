#include <boost/test/unit_test.hpp>

#include "support.hpp"
#include "tui/layout.hpp"

using namespace dagent::tui;
using test_support::row_text;

namespace {

// 探针控件：自然尺寸可调，render 用一个字符填满视图并计数。
class Probe : public Widget {
public:
    Probe(char32_t glyph, Size natural = {}) : glyph_(glyph), natural_(natural) {}

    Size measure(Size) const override { return natural_; }
    void render(Surface& s) override {
        ++renders;
        s.fill({0, 0, s.cols(), s.rows()}, glyph_, Style{});
    }
    void set_natural(Size n) {
        natural_ = n;
        invalidate_layout();
    }

    int renders = 0;

private:
    char32_t glyph_;
    Size natural_;
};

struct Tree {
    Container root;
    std::vector<Probe*> probes;

    explicit Tree(Container::Direction dir = Container::Direction::vertical) : root(dir) {}

    Probe& add(Constraint c, char32_t glyph, Size natural = {}) {
        auto p = std::make_unique<Probe>(glyph, natural);
        probes.push_back(p.get());
        root.add(c, std::move(p));
        return *probes.back();
    }
};

std::vector<Rect> rects(const Container& c) {
    std::vector<Rect> out;
    for (int i = 0; i < c.count(); ++i) out.push_back(c.child(i).rect());
    return out;
}

Constraint fixed(int n, int min = 0) { return {Sizing::fixed, n, min}; }
Constraint content(int min = 0, int max = 0x7FFFFFFF) { return {Sizing::content, 0, min, max}; }
Constraint flex(int weight = 1, int min = 0, int max = 0x7FFFFFFF) {
    return {Sizing::flex, weight, min, max};
}

} // namespace

BOOST_AUTO_TEST_SUITE(layout)

BOOST_AUTO_TEST_CASE(chat_layout_distribution) {
    Tree t;
    t.add(flex(), U's');
    t.add(content(0, 1), U'a', {0, 1});
    t.add(content(0, 1), U'n', {0, 0});
    t.add(content(3), U'i', {0, 3});
    t.root.layout({0, 0, 80, 24});
    BOOST_TEST(rects(t.root) == (std::vector<Rect>{{0, 0, 80, 20}, {0, 20, 80, 1},
                                                   {0, 21, 80, 0}, {0, 21, 80, 3}}));
}

BOOST_AUTO_TEST_CASE(content_is_clamped_to_min_and_max) {
    Tree t;
    t.add(content(2, 4), U'a', {0, 9});
    t.add(content(2, 4), U'b', {0, 0});
    t.root.layout({0, 0, 10, 10});
    BOOST_TEST(rects(t.root) == (std::vector<Rect>{{0, 0, 10, 4}, {0, 4, 10, 2}}));
}

BOOST_AUTO_TEST_CASE(flex_weights_share_remainder_in_declaration_order) {
    Tree t;
    t.add(flex(1), U'a');
    t.add(flex(2), U'b');
    t.root.layout({0, 0, 5, 10});
    BOOST_TEST(rects(t.root) == (std::vector<Rect>{{0, 0, 5, 4}, {0, 4, 5, 6}}));
}

BOOST_AUTO_TEST_CASE(flex_max_passes_leftover_to_growable_siblings) {
    Tree t;
    t.add(flex(1, 0, 2), U'a');
    t.add(flex(1), U'b');
    t.root.layout({0, 0, 5, 10});
    BOOST_TEST(rects(t.root) == (std::vector<Rect>{{0, 0, 5, 2}, {0, 2, 5, 8}}));
}

BOOST_AUTO_TEST_CASE(oversubscription_shrinks_in_reverse_order_down_to_min) {
    Tree t;
    t.add(fixed(6, 2), U'a');
    t.add(content(3), U'b', {0, 6});
    t.root.layout({0, 0, 5, 10});
    BOOST_TEST(rects(t.root) == (std::vector<Rect>{{0, 0, 5, 6}, {0, 6, 5, 4}}));

    Tree u;
    u.add(fixed(8), U'a');
    u.add(content(3), U'b', {0, 5});
    u.root.layout({0, 0, 5, 10});  // fixed 让出空间，content 不被压穿 min
    BOOST_TEST(rects(u.root) == (std::vector<Rect>{{0, 0, 5, 7}, {0, 7, 5, 3}}));
}

BOOST_AUTO_TEST_CASE(flex_min_is_honored_without_leaving_parent) {
    Tree t;
    t.add(flex(1, 4), U'a');
    t.add(content(), U'b', {0, 10});
    t.root.layout({0, 0, 5, 10});
    BOOST_TEST(rects(t.root) == (std::vector<Rect>{{0, 0, 5, 4}, {0, 4, 5, 6}}));

    Tree u;
    u.add(flex(1, 10), U'a');
    u.add(flex(1, 10), U'b');
    u.root.layout({0, 0, 5, 12});
    BOOST_TEST(rects(u.root) == (std::vector<Rect>{{0, 0, 5, 10}, {0, 10, 5, 2}}));
}

BOOST_AUTO_TEST_CASE(sum_of_min_beyond_parent_is_truncated_at_boundary) {
    Tree t;
    t.add(fixed(8, 8), U'a');
    t.add(fixed(8, 8), U'b');
    t.add(fixed(1, 1), U'c');
    t.root.layout({0, 0, 5, 10});
    BOOST_TEST(rects(t.root) == (std::vector<Rect>{{0, 0, 5, 8}, {0, 8, 5, 2}, {0, 10, 5, 0}}));
}

BOOST_AUTO_TEST_CASE(horizontal_uses_measured_columns) {
    Tree t(Container::Direction::horizontal);
    t.add(content(), U'a', {3, 99});
    t.add(flex(), U'b');
    t.add(fixed(2), U'c');
    t.root.layout({0, 0, 10, 4});
    BOOST_TEST(rects(t.root) == (std::vector<Rect>{{0, 0, 3, 4}, {3, 0, 5, 4}, {8, 0, 2, 4}}));
}

BOOST_AUTO_TEST_CASE(container_natural_size) {
    Container c;
    c.add(fixed(2), std::make_unique<Probe>(U'a'));
    c.add(content(), std::make_unique<Probe>(U'b', Size{5, 3}));
    c.add(flex(1, 1), std::make_unique<Probe>(U'c'));
    BOOST_TEST((c.measure({100, 100}) == Size{5, 6}));
}

BOOST_AUTO_TEST_CASE(children_render_into_their_own_regions) {
    Tree t;
    t.add(fixed(1), U'a');
    t.add(flex(), U'b');
    t.root.layout({0, 0, 3, 3});
    Surface s(3, 3);
    t.root.render(s);
    BOOST_TEST(row_text(s, 0) == "aaa");
    BOOST_TEST(row_text(s, 1) == "bbb");
    BOOST_TEST(row_text(s, 2) == "bbb");
}

BOOST_AUTO_TEST_CASE(only_invalidated_widgets_render_again) {
    Tree t;
    Probe& a = t.add(fixed(1), U'a');
    Probe& b = t.add(fixed(1), U'b');
    t.root.layout({0, 0, 4, 2});
    Surface s(4, 2);
    t.root.render(s);
    t.root.render(s);
    BOOST_TEST(a.renders == 1);
    BOOST_TEST(b.renders == 1);

    b.invalidate();
    BOOST_TEST(!t.root.needs_layout());
    t.root.render(s);
    BOOST_TEST(a.renders == 1);
    BOOST_TEST(b.renders == 2);

    t.root.invalidate_tree();
    t.root.render(s);
    BOOST_TEST(a.renders == 2);
    BOOST_TEST(b.renders == 3);
}

BOOST_AUTO_TEST_CASE(deep_invalidation_reaches_grandchild) {
    Container root;
    auto inner = std::make_unique<Container>(Container::Direction::horizontal);
    auto leaf = std::make_unique<Probe>(U'x');
    auto sibling = std::make_unique<Probe>(U'y');
    Probe& lp = *leaf;
    Probe& sp = *sibling;
    inner->add(flex(), std::move(leaf));
    root.add(flex(), std::move(inner));
    root.add(fixed(1), std::move(sibling));
    root.layout({0, 0, 6, 3});
    Surface s(6, 3);
    root.render(s);

    lp.set_natural({1, 1});  // 深层 invalidate_layout 沿树聚合
    BOOST_TEST(root.needs_layout());
    root.layout({0, 0, 6, 3});
    root.render(s);
    BOOST_TEST(lp.renders == 2);
    BOOST_TEST(sp.renders == 1);  // Rect 未变的兄弟不重画
}

BOOST_AUTO_TEST_CASE(moving_a_nested_container_repaints_its_subtree_at_new_position) {
    Container root;
    auto top = std::make_unique<Probe>(U'a');
    Probe& tp = *top;
    auto inner = std::make_unique<Container>(Container::Direction::horizontal);
    inner->add(flex(), std::make_unique<Probe>(U'x'));
    root.add(content(), std::move(top));
    root.add(fixed(2), std::move(inner));

    Surface front(3, 4), back(3, 4);
    root.layout({0, 0, 3, 4});
    root.render(back);
    std::swap(front, back);

    tp.set_natural({0, 1});
    back.copy_from(front);
    root.layout({0, 0, 3, 4});
    root.render(back);
    BOOST_TEST(row_text(back, 0) == "aaa");
    BOOST_TEST(row_text(back, 1) == "xxx");
    BOOST_TEST(row_text(back, 2) == "xxx");
    BOOST_TEST(row_text(back, 3) == "   ");
}

BOOST_AUTO_TEST_CASE(shrinking_widget_repaints_and_vacated_tail_is_cleared) {
    Tree t;
    Probe& p = t.add(content(), U'z', {0, 3});
    Surface front(2, 4), back(2, 4);
    t.root.layout({0, 0, 2, 4});
    t.root.render(back);
    std::swap(front, back);

    p.set_natural({0, 1});
    back.copy_from(front);
    t.root.layout({0, 0, 2, 4});
    t.root.render(back);
    BOOST_TEST(p.renders == 2);  // 尺寸变化即重画（控件内容可能依赖高度）
    BOOST_TEST(row_text(back, 0) == "zz");
    BOOST_TEST(row_text(back, 1) == "  ");
    BOOST_TEST(row_text(back, 2) == "  ");
    BOOST_TEST(row_text(back, 3) == "  ");
}

BOOST_AUTO_TEST_CASE(unchanged_layout_epoch_does_not_repaint_anything) {
    Tree t;
    Probe& a = t.add(flex(), U'a');
    Probe& b = t.add(content(), U'b', {0, 2});
    Surface s(2, 5);
    t.root.layout({0, 0, 2, 5});
    t.root.render(s);

    b.set_natural({0, 2});  // 声明可能变化，但尺寸实际没变
    t.root.layout({0, 0, 2, 5});
    t.root.render(s);
    BOOST_TEST(a.renders == 1);
    BOOST_TEST(b.renders == 2);  // 自己 invalidate_layout 仍会重画
}

BOOST_AUTO_TEST_SUITE_END()
