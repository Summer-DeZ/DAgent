#pragma once

#include <memory>

#include "tui/layout.hpp"
#include "tui/widget.hpp"

namespace dagent::ui {

/// 正文阅读栏宽度：maximum 为配置上限，0 表示自动。
void set_reading_width(int maximum) noexcept;
int reading_width(int available) noexcept;

/// 横跨整个主区域，左右各留 margin 列（输入框、活动行、排队行使用）。
std::unique_ptr<tui::Widget> spanning(std::unique_ptr<tui::Widget> child, int margin,
                                      const tui::ThemeTokens& theme = tui::dark_theme());

} // namespace dagent::ui
