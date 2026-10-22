#include "tui/widget/widget.hpp"

namespace dagent::tui {

namespace {

constexpr Style plain(Color fg, Attr attrs = Attr::none) noexcept {
    return Style{fg, Color{}, attrs};
}

/// @brief 底色令牌：颜色放 bg，fg 保持默认。
constexpr Style fill(Color bg) noexcept { return Style{Color{}, bg, Attr::none}; }

ThemeTokens make_theme(bool light) noexcept {
    ThemeTokens t;
    const auto color = [light](unsigned dark, unsigned day, int16_t dark256 = -1, int16_t light256 = -1) {
        const unsigned value = light ? day : dark;
        return Color::rgb((value >> 16) & 255, (value >> 8) & 255, value & 255, light ? light256 : dark256);
    };
    t.text = plain(color(0xe4e4e4, 0x1c1c1c));
    t.text_muted = plain(color(0xb2b2b2, 0x4e4e4e));
    t.text_faint = plain(color(0x8a8a8a, 0x6c6c6c));
    t.primary = plain(color(0xfe8a4c, 0xa85500));
    t.border = plain(color(0x3a3a3a, 0xc6c6c6));
    t.border_active = plain(color(0x585858, 0x9e9e9e));
    t.background = fill(color(0x121212, 0xffffff));
    t.background_panel = fill(color(0x1c1c1c, 0xeeeeee));
    t.background_element = fill(color(0x262626, 0xe4e4e4));
    t.background_selected = fill(color(0x303030, 0xdadada));
    t.success = plain(color(0x6eb6ff, 0x0261ca));
    t.error = plain(color(0xf24e6f, 0x9b0655));
    t.warning = plain(color(0xf1ce59, 0x4c4300));
    t.info = plain(color(0x5ad7d7, 0x007179));
    t.diff_added = plain(color(0xabcdff, 0x005c94));
    t.diff_removed = plain(color(0xf68486, 0x860f23));
    t.diff_added_bg = fill(color(0x21344f, 0xe1f1ff, 17, 195));
    t.diff_removed_bg = fill(color(0x381617, 0xfacecd, 52, 224));
    t.syntax_keyword = plain(color(0xc592e7, 0x8639c7));
    t.syntax_string = plain(color(0xa6d7a0, 0x297d0f));
    t.syntax_number = plain(color(0xf7a97c, 0x9a5505));
    t.syntax_function = plain(color(0x81a9fd, 0x215fbc));
    t.syntax_type = plain(color(0x81dbcb, 0x107a6c));
    t.markdown_code = plain(color(0xd8b0ff, 0x8b3bab));
    t.accent = plain(color(0xf99c6a, 0x8c4a06));
    t.diff_context = t.text;
    t.diff_hunk = t.text_faint;
    t.syntax_comment = plain(t.text_faint.fg, Attr::italic);
    t.markdown_heading = plain(t.text.fg, Attr::bold);
    t.markdown_link = plain(t.info.fg, Attr::underline);
    t.markdown_quote = plain(t.text_muted.fg, Attr::italic);
    t.selection = Style{t.background.bg, t.primary.fg, Attr::none};
    return t;
}

} // namespace

const ThemeTokens& dark_theme() noexcept {
    static const ThemeTokens tokens = make_theme(false);
    return tokens;
}

const ThemeTokens& light_theme() noexcept {
    static const ThemeTokens tokens = make_theme(true);
    return tokens;
}

float relative_luminance(const Color& c) noexcept {
    if (c.kind != Color::Kind::rgb) return 0.0f;
    return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f;
}

const ThemeTokens& default_theme(const std::optional<Color>& background) noexcept {
    return relative_luminance(background.value_or(Color{})) > 0.5f ? light_theme()
                                                                  : dark_theme();
}

} // namespace dagent::tui
