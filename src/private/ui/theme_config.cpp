#include "ui/theme_config.hpp"

#include <fstream>
#include <stdexcept>
#include <string_view>

#include "lib/nlohmann/json.hpp"

namespace dagent::ui {

namespace {

using nlohmann::json;
using tui::Attr;
using tui::Color;
using tui::Style;
using tui::ThemeTokens;

// 令牌名 → ThemeTokens 字段。与 widget.hpp 的 ThemeTokens 一一对应。
struct TokenField {
    std::string_view name;
    Style ThemeTokens::*field;
};
constexpr TokenField k_tokens[] = {
    {"text", &ThemeTokens::text},
    {"text_muted", &ThemeTokens::text_muted},
    {"primary", &ThemeTokens::primary},
    {"accent", &ThemeTokens::accent},
    {"border", &ThemeTokens::border},
    {"border_active", &ThemeTokens::border_active},
    {"background", &ThemeTokens::background},
    {"background_panel", &ThemeTokens::background_panel},
    {"background_element", &ThemeTokens::background_element},
    {"success", &ThemeTokens::success},
    {"warning", &ThemeTokens::warning},
    {"error", &ThemeTokens::error},
    {"info", &ThemeTokens::info},
    {"diff_added", &ThemeTokens::diff_added},
    {"diff_removed", &ThemeTokens::diff_removed},
    {"diff_context", &ThemeTokens::diff_context},
    {"diff_hunk", &ThemeTokens::diff_hunk},
    {"syntax_keyword", &ThemeTokens::syntax_keyword},
    {"syntax_string", &ThemeTokens::syntax_string},
    {"syntax_comment", &ThemeTokens::syntax_comment},
    {"syntax_number", &ThemeTokens::syntax_number},
    {"syntax_function", &ThemeTokens::syntax_function},
    {"syntax_type", &ThemeTokens::syntax_type},
    {"markdown_heading", &ThemeTokens::markdown_heading},
    {"markdown_code", &ThemeTokens::markdown_code},
    {"markdown_link", &ThemeTokens::markdown_link},
    {"markdown_quote", &ThemeTokens::markdown_quote},
    {"selection", &ThemeTokens::selection},
};

constexpr std::pair<std::string_view, Attr> k_attrs[] = {
    {"bold", Attr::bold},     {"dim", Attr::dim},       {"italic", Attr::italic},
    {"underline", Attr::underline}, {"blink", Attr::blink},
    {"reverse", Attr::reverse}, {"strike", Attr::strike},
};

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::runtime_error("invalid hex color in theme");
}

Color parse_color(const json& v, const json& defs) {
    if (v.is_number_integer()) return Color::indexed(v.get<uint8_t>());
    const std::string s = v.get<std::string>();
    if (s == "default") return {};
    if (const auto it = defs.find(s); it != defs.end()) return parse_color(*it, json::object());
    if (s.size() != 7 || s[0] != '#') throw std::runtime_error("unknown theme color: " + s);
    return Color::rgb(static_cast<uint8_t>(hex(s[1]) * 16 + hex(s[2])),
                      static_cast<uint8_t>(hex(s[3]) * 16 + hex(s[4])),
                      static_cast<uint8_t>(hex(s[5]) * 16 + hex(s[6])));
}

Style parse_style(const json& v, const json& defs) {
    if (!v.is_object()) return Style{parse_color(v, defs), {}, Attr::none};
    Style st;
    if (const auto it = v.find("fg"); it != v.end()) st.fg = parse_color(*it, defs);
    if (const auto it = v.find("bg"); it != v.end()) st.bg = parse_color(*it, defs);
    if (const auto it = v.find("attrs"); it != v.end()) {
        for (const json& a : *it) {
            const std::string name = a.get<std::string>();
            for (const auto& [attr_name, attr] : k_attrs) {
                if (name == attr_name) st.attrs = st.attrs | attr;
            }
        }
    }
    return st;
}

void apply(const json& tokens, const json& defs, ThemeTokens& out) {
    for (const TokenField& t : k_tokens) {
        if (const auto it = tokens.find(t.name); it != tokens.end()) {
            out.*t.field = parse_style(*it, defs);
        }
    }
}

} // namespace

const tui::ThemeTokens& ThemeSet::pick(const std::optional<tui::Color>& background) const {
    return &tui::default_theme(background) == &tui::light_theme() ? light : dark;
}

ThemeSet load_theme(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("cannot open theme file: " + file.string());
    const json j = json::parse(in);
    const json defs = j.value("defs", json::object());
    ThemeSet set{j.value("name", file.stem().string()), builtin_theme(false), builtin_theme(true)};
    if (const auto it = j.find("dark"); it != j.end()) apply(*it, defs, set.dark);
    if (const auto it = j.find("light"); it != j.end()) apply(*it, defs, set.light);
    return set;
}

tui::ThemeTokens builtin_theme(bool light) {
    auto theme = light ? tui::light_theme() : tui::dark_theme();
    const auto color = [light](unsigned dark, unsigned day) {
        const unsigned value = light ? day : dark;
        return Color::rgb((value >> 16) & 255, (value >> 8) & 255, value & 255);
    };
    const auto plain = [](Color fg, Attr attrs = Attr::none) { return Style{fg, {}, attrs}; };
    theme.text = plain(color(0xeeeeee, 0x1a1a1a));
    theme.text_muted = plain(color(0x808080, 0x706b66));
    theme.primary = plain(color(0xec5b2b, 0xc94d24));
    theme.accent = plain(color(0xee7948, 0xc94d24));
    theme.border = plain(color(0x3c3c3c, 0xd4d4d4));
    theme.border_active = plain(color(0x606060, 0xa0a0a0));
    theme.background = Style{{}, color(0x0a0a0a, 0xffffff), Attr::none};
    theme.background_panel = Style{{}, color(0x141414, 0xfff7f1), Attr::none};
    theme.background_element = Style{{}, color(0x1e1e1e, 0xf5f0eb), Attr::none};
    theme.success = plain(color(0x6ba1e6, 0x0062d1));
    theme.warning = plain(color(0xe5c07b, 0x8a6415));
    theme.error = plain(color(0xe06c75, 0xd1383d), Attr::bold);
    theme.info = plain(color(0x56b6c2, 0x318795));
    theme.diff_added = theme.success;
    theme.diff_removed = plain(theme.error.fg);
    theme.diff_context = theme.diff_hunk = theme.text_muted;
    theme.syntax_keyword = theme.primary;
    theme.syntax_string = theme.success;
    theme.syntax_comment = plain(theme.text_muted.fg, Attr::italic);
    theme.syntax_number = plain(color(0xfff7f1, 0xc94d24));
    theme.syntax_function = theme.accent;
    theme.syntax_type = theme.warning;
    theme.markdown_heading = plain(theme.primary.fg, Attr::bold);
    theme.markdown_code = theme.success;
    theme.markdown_link = plain(theme.info.fg, Attr::underline);
    theme.markdown_quote = plain(theme.text_muted.fg, Attr::italic);
    theme.selection = Style{theme.background.bg, theme.primary.fg, Attr::none};
    return theme;
}

tui::ThemeTokens resolve_theme(tui::ThemeTokens theme) {
    for (const auto& token : k_tokens) {
        if (token.field == &ThemeTokens::selection) continue;
        auto& style = theme.*token.field;
        if (style.bg.kind == Color::Kind::default_) style.bg = theme.background.bg;
        if (style.fg.kind == Color::Kind::default_) style.fg = theme.text.fg;
    }
    return theme;
}

} // namespace dagent::ui
