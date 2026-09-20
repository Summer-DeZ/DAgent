#include "ui/center.hpp"

#include <algorithm>

namespace dagent::ui {
namespace {
class CenterBox final : public tui::Widget {
public:
    CenterBox(std::unique_ptr<tui::Widget> child, int max_cols, const tui::ThemeTokens& theme)
        : child_(std::move(child)), max_cols_(max_cols), theme_(&theme) { adopt(*this, *child_); }
    ~CenterBox() override { disown(*child_); }

    tui::Size measure(tui::Size available) const override {
        const int width = content_width(available.cols);
        const tui::Size measured = child_->measure({width, available.rows});
        return {available.cols, measured.rows};
    }
    void layout(tui::Rect area) override {
        tui::Widget::layout(area);
        const int width = content_width(area.w);
        child_->layout({(area.w - width) / 2, 0, width, area.h});
    }
    void render(tui::Surface& surface) override {
        const auto area = child_->rect();
        surface.fill({0, 0, area.x, surface.rows()}, U' ', theme_->background);
        surface.fill({area.right(), 0, surface.cols() - area.right(), surface.rows()},
                     U' ', theme_->background);
        if (child_->dirty_tree() && !child_->rect().empty()) {
            auto view = surface.view(child_->rect()); child_->render(view); child_->clear_dirty();
        }
        clear_dirty();
    }
    bool dirty_tree() const noexcept override { return dirty_ || child_->dirty_tree(); }
    bool needs_layout() const noexcept override { return layout_dirty_ || child_->needs_layout(); }
    void invalidate_tree() noexcept override { invalidate(); child_->invalidate_tree(); }
    void invalidate_rect(tui::Rect rect) override { child_->invalidate_rect(rect); Widget::invalidate_rect(rect); }
    tui::Widget* hit_test(tui::Point screen) const noexcept override {
        if (auto* hit = child_->hit_test(screen)) return hit;
        return Widget::hit_test(screen);
    }
private:
    int content_width(int available) const noexcept {
        if (available < 44) return std::max(0, available);
        const int inset = std::max(0, available - 4);
        return max_cols_ > 0 ? std::min(max_cols_, inset) : inset;
    }
    std::unique_ptr<tui::Widget> child_;
    int max_cols_ = 88;
    const tui::ThemeTokens* theme_;
};
}

void Spacer::render(tui::Surface& surface) {
    surface.fill({0, 0, surface.cols(), surface.rows()}, U' ', {});
}

std::unique_ptr<tui::Widget> centered(std::unique_ptr<tui::Widget> child, int max_cols,
                                      const tui::ThemeTokens& theme) {
    return std::make_unique<CenterBox>(std::move(child), max_cols, theme);
}

} // namespace dagent::ui
