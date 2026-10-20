#include "ui/appearance.hpp"
#include "ui/theme_config.hpp"
#include "ui/display.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include "ui/strings.hpp"

namespace dagent::ui {
namespace {
const Icons sets[] = {
    {"◆", "⎇", "✓", "✗", "▸", "●", "○", "▣", "▣", "▌", "█", "│", "└", "─", "╭", "╮", "╰", "╯", "↑", "↓", "⚠", "−", " · ", "↵", "⇧↵",
     {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"}, "●", "!"},
    {"\uf489", "\ue0a0", "\uf00c", "\uf00d", "▸", "\uf111", "○", "\uf07b", "▣", "▌", "█", "│", "└", "─", "╭", "╮", "╰", "╯", "↑", "↓", "⚠", "−", " · ", "↵", "⇧↵",
     {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"}, "●", "!"},
    {"*", "|/", "v", "x", ">", "o", "o", "#", "#", "|", "#", "|", "+", "-", "+", "+", "+", "+", "^", "v", "!", "-", " . ", "enter", "shift+enter",
     {"-", "\\", "|", "/", "-", "\\", "|", "/", "-", "\\"}, "i", "!"},
};
int current_icons = 0;
tui::Color color(std::string_view hex) {
    const auto value = std::stoul(std::string(hex.substr(1)), nullptr, 16);
    return tui::Color::rgb((value >> 16) & 255, (value >> 8) & 255, value & 255);
}
tui::Color mix(tui::Color a, tui::Color b, double amount) {
    const auto channel = [amount](int x, int y) { return static_cast<uint8_t>(std::lround(x * (1 - amount) + y * amount)); };
    return tui::Color::rgb(channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b));
}
tui::Color rgb(tui::Color c) {
        if (c.kind != tui::Color::Kind::indexed) return c;
        const int n = c.r;
        if (n >= 232) return tui::Color::rgb(8 + (n - 232) * 10, 8 + (n - 232) * 10, 8 + (n - 232) * 10);
        if (n >= 16) {
            const auto channel = [](int v) { return v ? 55 + v * 40 : 0; };
            return tui::Color::rgb(channel((n - 16) / 36), channel((n - 16) / 6 % 6), channel((n - 16) % 6));
        }
        constexpr unsigned palette[] = {0x000000,0x800000,0x008000,0x808000,0x000080,0x800080,0x008080,0xc0c0c0,
                                        0x808080,0xff0000,0x00ff00,0xffff00,0x0000ff,0xff00ff,0x00ffff,0xffffff};
        return tui::Color::rgb(palette[n] >> 16, (palette[n] >> 8) & 255, palette[n] & 255);
}
}
Appearance Appearance::from_json(const nlohmann::json& j) {
    Appearance a;
    a.theme = j.value("theme", a.theme); a.mode = j.value("mode", a.mode);
    a.background = j.value("background", a.background); a.accent = j.value("accent", a.accent);
    a.panels = j.value("panels", a.panels); a.density = j.value("density", a.density);
    a.icons = j.value("icons", a.icons); a.sidebar = j.value("sidebar", a.sidebar);
    return a;
}
nlohmann::json Appearance::json() const {
    nlohmann::json j{{"theme", theme}, {"mode", mode}, {"background", background},
                     {"panels", panels}, {"density", density}, {"icons", icons}, {"sidebar", sidebar}};
    if (!accent.empty()) j["accent"] = accent;
    return j;
}
bool hex_color(std::string_view s) {
    return s.size() == 7 && s[0] == '#' && std::ranges::all_of(s.substr(1), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
tui::ThemeTokens appearance_theme(const Appearance& a, const std::filesystem::path& dir,
                                  const std::optional<tui::Color>& terminal) {
    const bool preset = a.theme == "dagent" || a.theme == "midnight" || a.theme == "paper";
    const auto set = load_theme(preset ? dir / (a.theme + ".json") : std::filesystem::path(a.theme));
    bool light = a.mode == "light" || (a.mode == "auto" && &set.pick(terminal) == &set.light);
    const bool custom = hex_color(a.background);
    if (custom) light = tui::relative_luminance(color(a.background)) > .5;
    auto t = light ? set.light : set.dark;
    if (a.background == "transparent") t.background.bg = {};
    if (custom) {
        t.background.bg = color(a.background);
        const auto toward = light ? tui::Color::rgb(0, 0, 0) : tui::Color::rgb(255, 255, 255);
        t.background_panel.bg = mix(t.background.bg, toward, light ? .03 : .05);
        t.background_element.bg = mix(t.background.bg, toward, light ? .06 : .10);
    }
    if (!a.accent.empty()) {
        t.primary.fg = color(a.accent); t.selection.bg = t.primary.fg;
        t.accent.fg = mix(t.primary.fg, t.text.fg, .2);
    }
    return t;
}
const Icons& icons() { return sets[current_icons]; }
void set_icons(std::string_view value) { current_icons = value == "ascii" ? 2 : value == "nerd" ? 1 : 0; }
void dim_surface(tui::Surface& s, const tui::ThemeTokens& t, std::optional<tui::Color> terminal) {
    auto background = t.background.bg;
    if (background.kind == tui::Color::Kind::default_) background = terminal.value_or(t.background_panel.bg);
    for (int y = 0; y < s.rows(); ++y) for (int x = 0; x < s.cols();) {
        const auto cell = s.at(x, y);
        if (cell.width == 0) { ++x; continue; }
        auto style = cell.style;
        auto foreground = style.fg.kind == tui::Color::Kind::default_ ? t.text.fg : style.fg;
        style.fg = mix(rgb(foreground), rgb(background), .6);
        s.put(x, y, cell.grapheme(), style);
        x += cell.width;
    }
}
void draw_frame(tui::Surface& s, tui::Rect r, tui::Style style) {
    if (r.w < 2 || r.h < 2) return;
    for (int x = r.x + 1; x < r.right() - 1; ++x) {
        s.put(x, r.y, icons().horizontal, style); s.put(x, r.bottom() - 1, icons().horizontal, style);
    }
    for (int y = r.y + 1; y < r.bottom() - 1; ++y) {
        s.put(r.x, y, icons().vertical, style); s.put(r.right() - 1, y, icons().vertical, style);
    }
    s.put(r.x, r.y, icons().top_left, style); s.put(r.right() - 1, r.y, icons().top_right, style);
    s.put(r.x, r.bottom() - 1, icons().bottom_left, style); s.put(r.right() - 1, r.bottom() - 1, icons().bottom_right, style);
}
int chip_width(std::string_view label, bool outline) { return display_width(label) + (outline ? 0 : 2); }
int draw_chip(tui::Surface& s, int x, int y, std::string_view label, tui::Style style,
              const tui::ThemeTokens& t, bool outline, tui::Color background) {
    style.bg = outline ? background : t.background_element.bg;
    return s.text(x, y, outline ? std::string(label) : " " + std::string(label) + " ", style);
}
std::string compact_tokens(std::size_t n) {
    if (n < 1000) return std::to_string(n);
    return n % 1000 == 0 ? std::format("{}k", n / 1000) : std::format("{:.1f}k", n / 1000.0);
}
tui::Color diff_background(const tui::ThemeTokens& t, bool added) {

    const auto base = t.background.bg.kind == tui::Color::Kind::default_ ? t.background_panel.bg : t.background.bg;
    return mix(rgb(base), rgb(added ? t.success.fg : t.error.fg), .15);
}
void Activity::render(tui::Surface& s) {
    s.fill({0, 0, s.cols(), s.rows()}, U' ', theme_->background);
    if (action_.empty()) return;
    const std::string hint = "esc " + std::string(text().hint_interrupt);
    auto value = action_;
    if (!tokens_.empty()) value += std::string(icons().separator) + tokens_;
    const auto without_rate = value;
    if (!rate_.empty()) value += std::string(icons().separator) + rate_;
    const int hint_width = display_width(hint);
    const auto fits = [&](std::string_view label) {
        return 2 + display_width(label) + 1 + hint_width <= s.cols();
    };
    if (!fits(value)) value = without_rate;
    if (!fits(value)) value = action_;
    const bool show_hint = fits(value);
    s.text(0, 0, icons().spinner[frame_], theme_->accent);
    if (2 + display_width(value) <= s.cols()) s.text(2, 0, value, theme_->text_muted);
    if (show_hint) s.text(s.cols() - hint_width, 0, hint, theme_->text_muted);
}
} // namespace dagent::ui
