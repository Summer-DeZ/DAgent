#include "app/config_detail.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

namespace dagent::app {
namespace fs = std::filesystem;
namespace {

using detail::ConfigDocument;
using detail::ModelWriteLock;
using detail::absolute_under;
using detail::fail;
using json = nlohmann::json;

} // namespace

nlohmann::json resolve_appearance(const nlohmann::json& input, const fs::path& root,
                                   const fs::path& legacy_theme) {
    if (!input.is_object()) fail(ConfigError::Kind::type, "/ui/appearance must be an object");
    json result{{"theme", legacy_theme.empty() ? "dagent" : legacy_theme.string()},
                {"mode", "auto"}, {"background", "solid"}, {"accent", "orange"}, {"panels", "filled"},
                {"density", "comfortable"}, {"icons", "unicode"}, {"sidebar", "right"}, {"diff", "auto"}, {"links", "on"}};
    const auto hex = [](const std::string& value) {
        return value.size() == 7 && value[0] == '#' &&
            std::ranges::all_of(value.substr(1), [](unsigned char c) { return std::isxdigit(c); });
    };
    const std::map<std::string, std::vector<std::string>> choices{
        {"mode", {"auto", "dark", "light"}}, {"panels", {"filled", "outline"}},
        {"density", {"comfortable", "compact"}}, {"icons", {"unicode", "nerd", "ascii"}},
        {"sidebar", {"right", "left", "hidden"}}, {"diff", {"auto", "unified"}}, {"links", {"on", "off"}}};
    for (auto it = input.begin(); it != input.end(); ++it) {
        const auto path = "/ui/appearance/" + it.key();
        if (!it->is_string()) fail(ConfigError::Kind::type, path + " must be a string");
        auto value = it->get<std::string>();
        if (const auto c = choices.find(it.key()); c != choices.end()) {
            if (std::ranges::find(c->second, value) == c->second.end())
                fail(ConfigError::Kind::invalid, path + ": invalid value '" + value + "'");
        } else if (it.key() == "background") {
            if (value != "solid" && value != "transparent" && !hex(value))
                fail(ConfigError::Kind::invalid, path + " must be solid, transparent or #rrggbb");
        } else if (it.key() == "accent") {
            if (hex(value)) {
                auto legacy = value;
                std::ranges::transform(legacy, legacy.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (legacy == "#ec5b2b") value = "orange";
                else if (legacy == "#6ba1e6") value = "blue";
                else if (legacy == "#c792ea") value = "violet";
                else if (legacy == "#7fd17f") value = "green";
            } else if (value != "orange" && value != "green" && value != "violet" && value != "blue") {
                fail(ConfigError::Kind::invalid, path + " must be orange, green, violet, blue or #rrggbb");
            }
        } else if (it.key() == "theme") {
            if (value.empty()) fail(ConfigError::Kind::invalid, path + " must be a preset name or a theme file path");
        } else {
            fail(ConfigError::Kind::invalid, "unknown appearance key: " + path);
        }
        result[it.key()] = value;
    }
    const auto name = result["theme"].get<std::string>();
    if (name == "midnight" || name == "paper")
        fail(ConfigError::Kind::invalid, "/ui/appearance/theme: midnight and paper presets were removed; use dagent or a theme file path");
    const auto file = name == "dagent"
        ? root / "themes" / (name + ".json") : absolute_under(root, name);
    if (!fs::is_regular_file(file)) fail(ConfigError::Kind::invalid, "/ui/appearance/theme: cannot open " + file.string());
    result["theme"] = file.string();
    return result;
}

nlohmann::json resolve_layout(const nlohmann::json& input, const nlohmann::json& appearance) {
    if (!input.is_object()) fail(ConfigError::Kind::type, "/ui/layout must be an object");
    json out{{"width", "auto"}, {"status_panel", appearance.value("sidebar", "right")}, {"navigator", "auto"},
             {"navigator_sections", {"sessions", "git", "skills", "agents"}},
             {"status_sections", {"session", "context", "plan", "changed", "running_agents", "mcp"}}};
    for (auto it = input.begin(); it != input.end(); ++it) {
        const auto path = "/ui/layout/" + it.key();
        if (!out.contains(it.key())) fail(ConfigError::Kind::invalid, "unknown layout key: " + path);
        if (it.key() == "width") {
            const bool number = it->is_number_integer() && *it >= 60 && *it <= 240;
            if (!number && *it != "auto" && *it != "compact" && *it != "wide")
                fail(ConfigError::Kind::invalid, path + ": expected auto, compact, wide or integer 60–240");
        } else if (it.key() == "navigator") {
            if (*it != "auto" && *it != "always" && *it != "hidden") fail(ConfigError::Kind::invalid, path + ": expected auto, always or hidden");
        } else if (it.key() == "status_panel") {
            if (*it != "left" && *it != "right" && *it != "hidden") fail(ConfigError::Kind::invalid, path + ": expected left, right or hidden");
        } else if (!it->is_array()) fail(ConfigError::Kind::type, path + ": expected array");
        out[it.key()] = *it;
    }
    std::set<std::string> seen;
    const std::set<std::string> ids{"sessions", "git", "skills", "agents", "session", "context", "plan", "changed", "running_agents", "mcp"};
    for (const auto key : {"navigator_sections", "status_sections"}) {
        for (const auto& id : out[key]) {
            if (!id.is_string() || !ids.contains(id.get<std::string>()))
                fail(ConfigError::Kind::invalid, std::string("/ui/layout/") + key + ": unknown section id");
            if (!seen.insert(id.get<std::string>()).second)
                fail(ConfigError::Kind::invalid, std::string("/ui/layout/") + key + ": duplicate section " + id.get<std::string>());
        }
    }
    return out;
}

nlohmann::json save_appearance(const fs::path& root, const nlohmann::json& appearance,
                               const nlohmann::json& layout) {
    ModelWriteLock lock(root);
    const fs::path file = root / "config" / "config.json";
    ConfigDocument document(file);
    const auto effective = resolve_appearance(appearance, root, {});
    const auto defaults = resolve_appearance(json::object(), root, {});
    auto ui = document.value.value("ui", ConfigDocument::Ordered::object());
    auto saved = ui.value("appearance", ConfigDocument::Ordered::object());
    for (auto it = appearance.begin(); it != appearance.end(); ++it) {
        if (saved.contains(it.key()) || effective[it.key()] != defaults[it.key()])
            saved[it.key()] = it.key() == "accent" ? effective[it.key()] : *it;
    }
    if (!layout.is_null()) {
        resolve_layout(layout, effective);
        saved.erase("sidebar");
        const auto layout_defaults = resolve_layout(json::object(), json::object());
        auto saved_layout = ui.value("layout", ConfigDocument::Ordered::object());
        for (auto it = layout.begin(); it != layout.end(); ++it) {
            if (saved_layout.contains(it.key()) || *it != layout_defaults[it.key()]) saved_layout[it.key()] = *it;
        }
        if (!saved_layout.empty() || ui.contains("layout")) ui["layout"] = std::move(saved_layout);
    }
    if (!saved.empty() || ui.contains("appearance")) ui["appearance"] = std::move(saved);
    if (!ui.empty()) document.value["ui"] = std::move(ui);
    workspace::write_text(file, document.dump(), workspace::Eol::lf, false, workspace::stamp_of(file));
    return effective;
}

} // namespace dagent::app
