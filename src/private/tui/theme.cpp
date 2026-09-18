#include "tui/widget.hpp"

namespace dagent::tui {

namespace {

constexpr Style plain(Color fg, Attr attrs = Attr::none) noexcept {
    return Style{fg, Color{}, attrs};
}

/// @brief 底色令牌：颜色放 bg，fg 保持默认。
constexpr Style fill(Color bg) noexcept { return Style{Color{}, bg, Attr::none}; }

ThemeTokens make_dark() noexcept {
    ThemeTokens t;
    t.text = Style{};
    t.text_muted = plain(Color::indexed(244));
    t.primary = plain(Color::indexed(111));
    t.accent = plain(Color::indexed(180));
    t.border = plain(Color::indexed(240));
    t.border_active = plain(Color::indexed(111));
    t.background = Style{}; // 不主动刷底色，保留终端自带背景
    t.background_panel = fill(Color::indexed(236));
    t.background_element = fill(Color::indexed(235));
    t.success = plain(Color::indexed(114));
    t.warning = plain(Color::indexed(179));
    t.error = plain(Color::indexed(203), Attr::bold);
    t.info = plain(Color::indexed(75));
    t.diff_added = plain(Color::indexed(114));
    t.diff_removed = plain(Color::indexed(203));
    t.diff_context = plain(Color::indexed(244));
    t.diff_hunk = plain(Color::indexed(111));
    t.syntax_keyword = plain(Color::indexed(176), Attr::bold);
    t.syntax_string = plain(Color::indexed(114));
    t.syntax_comment = plain(Color::indexed(244));
    t.syntax_number = plain(Color::indexed(180));
    t.syntax_function = plain(Color::indexed(111));
    t.syntax_type = plain(Color::indexed(180));
    t.markdown_heading = plain(Color::indexed(111), Attr::bold);
    t.markdown_code = plain(Color::indexed(114));
    t.markdown_link = plain(Color::indexed(75), Attr::underline);
    t.markdown_quote = plain(Color::indexed(244));
    t.selection = Style{Color{}, Color{}, Attr::reverse};
    return t;
}

ThemeTokens make_light() noexcept {
    ThemeTokens t;
    t.text = Style{};
    t.text_muted = plain(Color::indexed(240));
    t.primary = plain(Color::indexed(26));
    t.accent = plain(Color::indexed(130));
    t.border = plain(Color::indexed(250));
    t.border_active = plain(Color::indexed(26));
    t.background = Style{};
    t.background_panel = fill(Color::indexed(254));
    t.background_element = fill(Color::indexed(255));
    t.success = plain(Color::indexed(28));
    t.warning = plain(Color::indexed(130));
    t.error = plain(Color::indexed(160), Attr::bold);
    t.info = plain(Color::indexed(26));
    t.diff_added = plain(Color::indexed(28));
    t.diff_removed = plain(Color::indexed(160));
    t.diff_context = plain(Color::indexed(240));
    t.diff_hunk = plain(Color::indexed(26));
    t.syntax_keyword = plain(Color::indexed(90), Attr::bold);
    t.syntax_string = plain(Color::indexed(28));
    t.syntax_comment = plain(Color::indexed(245));
    t.syntax_number = plain(Color::indexed(130));
    t.syntax_function = plain(Color::indexed(26));
    t.syntax_type = plain(Color::indexed(130));
    t.markdown_heading = plain(Color::indexed(26), Attr::bold);
    t.markdown_code = plain(Color::indexed(28));
    t.markdown_link = plain(Color::indexed(26), Attr::underline);
    t.markdown_quote = plain(Color::indexed(240));
    t.selection = Style{Color{}, Color{}, Attr::reverse};
    return t;
}

} // namespace

const ThemeTokens& dark_theme() noexcept {
    static const ThemeTokens tokens = make_dark();
    return tokens;
}

const ThemeTokens& light_theme() noexcept {
    static const ThemeTokens tokens = make_light();
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
