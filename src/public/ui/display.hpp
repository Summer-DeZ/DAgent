#pragma once

#include <string>
#include <string_view>

namespace dagent::ui {
// 所有应用层列表按终端字素列宽排版，不能使用 UTF-8 字节数。
int display_width(std::string_view);
std::string fit_columns(std::string_view, int columns);
}
