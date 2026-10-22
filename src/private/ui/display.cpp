#include "ui/display.hpp"

#include <algorithm>
#include <format>

#include "tui/grid/grapheme.hpp"

namespace dagent::ui {
std::string compact_tokens(std::size_t n) {
    if (n < 1000) return std::to_string(n);
    return n % 1000 == 0 ? std::format("{}k", n / 1000) : std::format("{:.1f}k", n / 1000.0);
}

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
