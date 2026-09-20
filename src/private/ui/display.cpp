#include "ui/display.hpp"

#include <algorithm>

#include "tui/grapheme.hpp"

namespace dagent::ui {
int display_width(std::string_view text) {
    int width = 0;
    tui::unicode::Grapheme g;
    while (tui::unicode::next_grapheme(text, g)) width += g.width;
    return width;
}
std::string fit_columns(std::string_view text, int columns) {
    if (columns <= 0) return {};
    if (display_width(text) <= columns) return std::string(text);
    const std::string suffix(static_cast<std::size_t>(std::min(columns, 3)), '.');
    std::string result;
    int width = 0;
    tui::unicode::Grapheme g;
    while (tui::unicode::next_grapheme(text, g)) {
        if (width + g.width > columns - static_cast<int>(suffix.size())) break;
        result.append(g.bytes);
        width += g.width;
    }
    return result + suffix;
}
}
