#pragma once

#include <memory>

#include "tui/layout.hpp"
#include "tui/widget.hpp"

namespace dagent::ui {

/// 空占位；由 Container 的 flex 约束吸收内容栏两侧空间。
class Spacer final : public tui::Widget {
public:
    tui::Size measure(tui::Size) const override { return {}; }
    void render(tui::Surface&) override;
};

/// 把控件放进最多 max_cols 列的居中栏。max_cols == 0 表示不限宽。
std::unique_ptr<tui::Widget> centered(std::unique_ptr<tui::Widget> child,
                                      int max_cols = 88,
                                      const tui::ThemeTokens& theme = tui::dark_theme());

} // namespace dagent::ui
