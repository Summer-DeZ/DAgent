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
    {"◆", "⎇", "✓", "✗", "▸", "●", "○", "●", "●", "▌", "█", "│", "└", "─", "╭", "╮", "╰", "╯", "↑", "↓", "⚠", "−", " · ", "↵", "⇧↵",
     {"◐", "◓", "◑", "◒"}, "●", "!", "→", "›", "●", "⎿", "✻", "›", "☐", "☒"},
    {"\uf489", "\ue0a0", "\uf00c", "\uf00d", "▸", "\uf111", "○", "\uf07b", "●", "▌", "█", "│", "└", "─", "╭", "╮", "╰", "╯", "↑", "↓", "⚠", "−", " · ", "↵", "⇧↵",
     {"◐", "◓", "◑", "◒"}, "●", "!", "→", "›", "●", "⎿", "✻", "›", "☐", "☒"},
    {"*", "|/", "v", "x", ">", "o", "o", "#", "#", "|", "#", "|", "+", "-", "+", "+", "+", "+", "^", "v", "!", "-", " . ", "enter", "shift+enter",
     {"|", "/", "-", "\\"}, "i", "!", ">", ">", "*", "`-", "~", ">", "[ ]", "[x]"},
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
// 在 OKLab 中沿固定色相调整明度；超出 sRGB 色域时只缩小色度。
double linear(double value) {
    return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
}
double luminance(tui::Color value) {
    value = rgb(value);
    return .2126 * linear(value.r / 255.) + .7152 * linear(value.g / 255.) + .0722 * linear(value.b / 255.);
}
tui::Color contrast_accent(tui::Color value, tui::Color panel, bool light) {
    const double base = luminance(panel);
    const auto readable = [base](tui::Color c) {
        const double lum = luminance(c);
        return (std::max(lum, base) + .05) / (std::min(lum, base) + .05) >= 4.5;
    };
    if (readable(value)) return value;
    const double r = linear(value.r / 255.), g = linear(value.g / 255.), b = linear(value.b / 255.);
    const double l = std::cbrt(.4122214708*r + .5363325363*g + .0514459929*b);
    const double m = std::cbrt(.2119034982*r + .6806995451*g + .1073969566*b);
    const double s = std::cbrt(.0883024619*r + .2817188376*g + .6299787005*b);
    const double original = .2104542553*l + .7936177850*m - .0040720468*s;
    const double a = 1.9779984951*l - 2.4285922050*m + .4505937099*s;
    const double bb = .0259040371*l + .7827717662*m - .8086757660*s;
    const auto at_lightness = [a, bb](double level) {
        const auto channels = [=](double chroma) {
            const double ll = std::pow(level + .3963377774*a*chroma + .2158037573*bb*chroma, 3);
            const double mm = std::pow(level - .1055613458*a*chroma - .0638541728*bb*chroma, 3);
            const double ss = std::pow(level - .0894841775*a*chroma - 1.2914855480*bb*chroma, 3);
            return std::array{4.0767416621*ll - 3.3077115913*mm + .2309699292*ss,
                             -1.2684380046*ll + 2.6097574011*mm - .3413193965*ss,
                             -.0041960863*ll - .7034186147*mm + 1.7076147010*ss};
        };
        auto result = channels(1);
        const auto in_gamut = [](const auto& values) {
            return std::ranges::all_of(values, [](double c) { return c >= 0 && c <= 1; });
        };
        if (!in_gamut(result)) {
            double low = 0, high = 1;
            for (int i = 0; i < 24; ++i) {
                const double mid = (low + high) / 2;
                if (in_gamut(channels(mid))) low = mid;
                else high = mid;
            }
            result = channels(low);
        }
        const auto encode = [](double c) {
            c = std::clamp(c, 0., 1.);
            return static_cast<uint8_t>(std::lround(255 * (c <= .0031308 ? 12.92*c : 1.055*std::pow(c, 1/2.4) - .055)));
        };
        return tui::Color::rgb(encode(result[0]), encode(result[1]), encode(result[2]));
    };
    double failing = original, passing = light ? 0 : 1;
    auto adjusted = at_lightness(passing);
    for (int i = 0; i < 24; ++i) {
        const double mid = (failing + passing) / 2;
        const auto candidate = at_lightness(mid);
        if (readable(candidate)) { passing = mid; adjusted = candidate; }
        else failing = mid;
    }
    return adjusted;
}

}
Appearance Appearance::from_json(const nlohmann::json& j) {
    Appearance a;
    a.theme = j.value("theme", a.theme); a.mode = j.value("mode", a.mode);
    a.background = j.value("background", a.background); a.accent = j.value("accent", a.accent);
    a.panels = j.value("panels", a.panels); a.density = j.value("density", a.density);
    a.icons = j.value("icons", a.icons); a.sidebar = j.value("sidebar", a.sidebar);
    a.diff = j.value("diff", a.diff); a.links = j.value("links", a.links);
    return a;
}
nlohmann::json Appearance::json() const {
    nlohmann::json j{{"theme", theme}, {"mode", mode}, {"background", background},
                     {"panels", panels}, {"density", density}, {"icons", icons}, {"sidebar", sidebar}, {"diff", diff}, {"links", links}};
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
    const bool preset = a.theme == "dagent";
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
        t.background_selected.bg = mix(t.background.bg, toward, light ? .09 : .15);
    }
    if (a.background == "transparent" || custom) {
        const auto base = rgb(a.background == "transparent" ? t.background_panel.bg : t.background.bg);
        t.diff_added_bg.bg = mix(base, rgb(t.success.fg), .15);
        t.diff_removed_bg.bg = mix(base, rgb(t.error.fg), .15);
    }
    if (hex_color(a.accent)) t.primary.fg = contrast_accent(color(a.accent), t.background_panel.bg, light);
    else if (a.accent == "green") t.primary.fg = color(light ? "#2a7a1a" : "#87d787");
    else if (a.accent == "violet") t.primary.fg = color(light ? "#6a4bc4" : "#b9a0ff");
    else if (a.accent == "blue") t.primary.fg = color(light ? "#3157c8" : "#7a9cff");
    else t.primary.fg = color(light ? "#a85500" : "#fe8a4c");
    t.selection.bg = t.primary.fg;
    t.selection.fg = t.background.bg.kind == tui::Color::Kind::default_
        ? (light ? set.light : set.dark).background.bg : t.background.bg;
    t.accent.fg = mix(t.primary.fg, rgb(t.text.fg), .2);
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
void draw_hint(tui::Surface& s, int x, int y, std::string_view label, tui::Style style, tui::Style separator) {
    separator.bg = style.bg;
    while (!label.empty()) {
        const auto at = label.find("·");
        const auto ascii = label.find(" . ");
        const auto split = std::min(at, ascii);
        x = s.text(x, y, label.substr(0, split), style);
        if (split == std::string_view::npos) break;
        const size_t size = split == ascii ? 3 : std::string_view("·").size();
        x = s.text(x, y, label.substr(split, size), separator);
        label.remove_prefix(split + size);
    }
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
    return (added ? t.diff_added_bg : t.diff_removed_bg).bg;
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
