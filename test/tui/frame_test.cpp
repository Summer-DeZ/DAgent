// L3/L7 帧管线验收（02-tui-final-update §3.4.2）：损伤传播。
//   * 增量 == 全量：随机打开/移动/关闭浮层、随机失效基础层、改变尺寸后，
//     增量路径得到的 back 与对同一棵树 invalidate_tree() 后全量渲染的
//     结果逐格相同（本节核心断言）；
//   * 关闭覆盖输入框的对话框后，差分输出只包含对话框原矩形内的行；
//   * 浮层盖住的空白（gap）区域在关闭后恢复空白。

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <memory>
#include <ostream>
#include <random>
#include <string>
#include <vector>

#include "tui/layout.hpp"
#include "tui/surface.hpp"
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
    void set_char(char32_t ch) {
        ch_ = ch;
        invalidate();
    }

private:
    int w_;
    int h_;
    char32_t ch_;
};

// 从差分字节里取出所有 CUP 的行号（0 基）。
std::vector<int> cup_rows(std::string_view bytes) {
    std::vector<int> rows;
    for (std::size_t i = 0; i + 3 < bytes.size(); ++i) {
        if (bytes[i] != '\x1b' || bytes[i + 1] != '[') continue;
        std::size_t j = i + 2;
        if (bytes[j] < '0' || bytes[j] > '9') continue;
        int row = 0;
        while (j < bytes.size() && bytes[j] >= '0' && bytes[j] <= '9') {
            row = row * 10 + (bytes[j] - '0');
            ++j;
        }
        if (j >= bytes.size() || bytes[j] != ';') continue;
        ++j;
        if (j >= bytes.size() || bytes[j] < '0' || bytes[j] > '9') continue;
        while (j < bytes.size() && bytes[j] >= '0' && bytes[j] <= '9') ++j;
        if (j < bytes.size() && bytes[j] == 'H') rows.push_back(row - 1);
    }
    return rows;
}

} // namespace

namespace {

constexpr int k_cols = 40;
constexpr int k_rows = 12;

// 单轮随机序列：每一步后断言增量渲染与整树全量重画逐格相同。
// 随机损伤传播的缺陷对种子敏感，调用方用多个种子重复跑。
void random_round(std::uint32_t seed, int steps) {
    auto base = std::make_unique<Container>(Container::Direction::vertical);
    std::vector<Text*> texts;
    for (int i = 0; i < 2; ++i) {
        auto t = std::make_unique<Text>();
        t->set_text(std::string(static_cast<std::size_t>(i) + 1, 'a' + i));
        texts.push_back(t.get());
        base->add({Sizing::fixed, 2}, std::move(t)); // 只占上部：底部留 gap
    }
    auto content = std::make_unique<Text>();
    content->set_text("content");
    texts.push_back(content.get());
    // content 尺寸随行数变化：让"改内容"也能触发重新布局。
    base->add({Sizing::content, 0, 1, 4}, std::move(content));
    LayerStack stack{std::move(base)};
    stack.layout({0, 0, k_cols, k_rows});

    int cols = k_cols;
    int rows = k_rows;
    Surface front(cols, rows);
    Surface back(cols, rows);
    Surface ref(cols, rows);

    std::vector<uint32_t> ids;
    std::vector<Block*> blocks;
    std::mt19937 rng(seed);
    const Placement places[] = {Placement::center, Placement::top_right,
                                Placement::above_point, Placement::at_point};

    for (int step = 0; step < steps; ++step) {
        switch (rng() % 8) {
        case 0: { // 打开浮层
            const int w = 3 + static_cast<int>(rng() % 18);
            const int h = 2 + static_cast<int>(rng() % 5);
            auto b = std::make_unique<Block>(
                w, h, static_cast<char32_t>(U'A' + rng() % 26));
            blocks.push_back(b.get());
            ids.push_back(stack.push(
                std::move(b), places[rng() % 4],
                Point{static_cast<int>(rng() % cols),
                      static_cast<int>(rng() % rows)}));
            break;
        }
        case 1: // 移动浮层
            if (!ids.empty()) {
                const std::size_t i = rng() % ids.size();
                stack.move(ids[i], places[rng() % 4],
                           Point{static_cast<int>(rng() % cols),
                                 static_cast<int>(rng() % rows)});
            }
            break;
        case 2: // 关闭浮层
            if (!ids.empty()) {
                const std::size_t i = rng() % ids.size();
                stack.remove(ids[i]);
                ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(i));
                blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
            }
            break;
        case 3: { // 基础层改内容（可能改变行数 → 布局纪元）
            const int lines = static_cast<int>(rng() % 5);
            std::string s;
            for (int i = 0; i < lines; ++i) {
                if (i) s += '\n';
                s += std::string(static_cast<std::size_t>(rng() % 12), 'x');
            }
            texts[rng() % texts.size()]->set_text(std::move(s));
            break;
        }
        case 4: // 基础层仅失效
            texts[rng() % texts.size()]->invalidate();
            break;
        case 5: // 浮层改内容
            if (!blocks.empty()) {
                blocks[rng() % blocks.size()]->set_char(
                    static_cast<char32_t>(U'a' + rng() % 26));
            }
            break;
        case 6: // 整树失效（主题切换）
            stack.invalidate_tree();
            break;
        case 7: // 改变尺寸：resize 纪元（front/back 重置为空白 + 整树补画）
            cols = 20 + static_cast<int>(rng() % 30);
            rows = 8 + static_cast<int>(rng() % 8);
            front.resize(cols, rows);
            back.resize(cols, rows);
            ref.resize(cols, rows);
            stack.layout({0, 0, cols, rows});
            stack.invalidate_tree();
            break;
        default:
            break;
        }

        // 增量路径：与 Runtime::frame 相同的缓冲语义。
        if (stack.needs_layout()) stack.layout({0, 0, cols, rows});
        back.copy_from(front);
        stack.render(back);

        // 全量参考：空白起点 + invalidate_tree + 整树重画。终端全量补画
        // （resize 纪元也是这样）不会保留旧的 gap 像素，增量路径的损伤
        // 擦除必须与它一致。
        ref.clear();
        stack.invalidate_tree();
        stack.render(ref);

        int diff = 0;
        std::string where;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (back.at(c, r) == ref.at(c, r)) continue;
                if (diff == 0) {
                    where = "first diff at col " + std::to_string(c) +
                            ", row " + std::to_string(r);
                }
                ++diff;
            }
        }
        BOOST_TEST_CONTEXT("seed " << seed << " step " << step << " " << where) {
            BOOST_TEST(diff == 0);
        }
        if (diff != 0) break;

        std::swap(front, back);
        back.clear_dirty();
    }
}

} // namespace

BOOST_AUTO_TEST_SUITE(frame)

BOOST_AUTO_TEST_CASE(incremental_matches_full_after_random_overlay_ops) {
    for (const std::uint32_t seed :
         {1u, 0x0420u, 0xC0FFEEu, 0xDEADBEEFu, 0x5EEDu, 7u}) {
        random_round(seed, 250);
    }
}

BOOST_AUTO_TEST_CASE(closing_overlay_diff_is_confined_to_old_rect) {
    auto base = std::make_unique<Container>(Container::Direction::vertical);
    base->add({Sizing::fixed, 3}, std::make_unique<InputBox>());
    auto body = std::make_unique<Text>();
    body->set_text("body");
    base->add({Sizing::flex, 1}, std::move(body));
    LayerStack stack{std::move(base)};
    stack.layout({0, 0, 40, 10});

    auto dialog = std::make_unique<Block>(20, 8, U'D');
    Block* dialog_p = dialog.get();
    const uint32_t id = stack.push(std::move(dialog), Placement::at_point, {4, 0});
    const Rect old = dialog_p->rect();
    BOOST_REQUIRE(old == (Rect{4, 0, 20, 8})); // 对话框盖住输入框

    Surface front(40, 10);
    Surface back(40, 10);
    stack.invalidate_tree();
    back.copy_from(front);
    stack.render(back); // 首帧：基础层 + 对话框
    std::swap(front, back);
    back.clear_dirty();
    BOOST_TEST(front.at(5, 1).grapheme() == "D");

    std::unique_ptr<Widget> removed = stack.remove(id);
    BOOST_REQUIRE(removed != nullptr);
    back.copy_from(front);
    stack.render(back); // 增量帧：只剩基础层

    // 逐格比较：旧矩形之外的单元格必须没有变化。
    int outside = 0;
    for (int r = 0; r < 10; ++r) {
        for (int c = 0; c < 40; ++c) {
            if (back.at(c, r) == front.at(c, r)) continue;
            if (!old.contains({c, r})) ++outside;
        }
    }
    BOOST_TEST(outside == 0);

    // 差分输出只包含旧矩形内的行。
    std::string out;
    render_frame(out, back, front, FrameOptions{});
    const std::vector<int> rows = cup_rows(out);
    BOOST_REQUIRE(!rows.empty());
    int bad_rows = 0;
    for (const int r : rows) {
        if (r < old.y || r >= old.bottom()) ++bad_rows;
    }
    BOOST_TEST(bad_rows == 0);
}

BOOST_AUTO_TEST_CASE(damage_over_uncovered_gap_restores_blank) {
    auto base = std::make_unique<Container>(Container::Direction::vertical);
    auto top = std::make_unique<Text>();
    top->set_text("top");
    base->add({Sizing::fixed, 2}, std::move(top));
    LayerStack stack{std::move(base)};
    stack.layout({0, 0, 20, 8}); // 行 2..7 无人认领（gap）

    auto popup = std::make_unique<Block>(10, 4, U'P');
    Block* popup_p = popup.get();
    const uint32_t id =
        stack.push(std::move(popup), Placement::at_point, {2, 3});
    BOOST_TEST(popup_p->rect() == (Rect{2, 3, 10, 4}));

    Surface front(20, 8);
    Surface back(20, 8);
    stack.invalidate_tree();
    back.copy_from(front);
    stack.render(back);
    std::swap(front, back);
    back.clear_dirty();
    BOOST_TEST(front.at(3, 4).grapheme() == "P");

    stack.remove(id);
    back.copy_from(front);
    stack.render(back);
    // gap 区域没有控件补画：损伤矩形直接被擦成空白。
    BOOST_TEST(back.at(3, 4).grapheme() == " ");
    BOOST_TEST(back.at(11, 6).grapheme() == " ");
    BOOST_TEST(back.at(0, 0).grapheme() == "t"); // 基础层内容不受影响
}

BOOST_AUTO_TEST_SUITE_END()
