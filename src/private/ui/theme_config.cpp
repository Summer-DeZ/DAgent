#include "ui/theme_config.hpp"

#include <fstream>
#include <algorithm>
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
    theme.text.fg = light ? Color::rgb(52, 59, 88) : Color::rgb(212, 216, 227);
    theme.background.bg = light ? Color::rgb(255, 255, 255) : Color::rgb(26, 27, 38);
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

std::vector<ThemeInfo> list_themes(const std::filesystem::path& directory) {
    std::vector<ThemeInfo> result;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".json") continue;
        ThemeInfo info;
        info.path = entry.path();
        try { info.loaded = load_theme(info.path); info.name = info.loaded->name; }
        catch (...) { info.name = info.path.stem().string(); info.available = false; }
        result.push_back(std::move(info));
    }
    std::ranges::sort(result, {}, &ThemeInfo::name);
    return result;
}

} // namespace dagent::ui
