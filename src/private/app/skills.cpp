#include "app/skills.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>

#include <yaml-cpp/yaml.h>

#include "base/log.hpp"

namespace dagent::app {
namespace {
namespace fs = std::filesystem;

bool under(const fs::path& file, const fs::path& directory) {
    const auto relative = file.lexically_relative(directory);
    return !relative.empty() && *relative.begin() != "..";
}

std::string scalar(const YAML::Node& node, const char* key, bool required = false) {
    const auto value = node[key];
    if (!value && !required) return {};
    if (!value || !value.IsScalar()) throw std::runtime_error(std::string(key) + " must be a string");
    return value.as<std::string>();
}

std::size_t characters(std::string_view value) {
    return std::ranges::count_if(value, [](unsigned char c) { return (c & 0xc0) != 0x80; });
}

agent::SkillDefinition parse(const fs::path& file, const workspace::FileOptions& options) {
    const auto input = workspace::read_text(file, options);
    if (input.truncated || input.lossy) throw std::runtime_error("SKILL.md must fit the file read limit and contain valid UTF-8");
    const std::string& text = input.content;
    auto line = [&](std::size_t start, std::size_t end) {
        auto value = std::string_view(text).substr(start, end - start);
        if (value.ends_with('\r')) value.remove_suffix(1);
        return value;
    };
    const auto first = text.find('\n');
    if (first == std::string::npos || line(0, first) != "---")
        throw std::runtime_error("SKILL.md must begin with YAML frontmatter (---)");
    std::size_t end = first + 1, body = std::string::npos;
    while (end < text.size()) {
        const auto newline = text.find('\n', end);
        const auto last = newline == std::string::npos ? text.size() : newline;
        if (line(end, last) == "---") { body = newline == std::string::npos ? last : newline + 1; break; }
        if (newline == std::string::npos) break;
        end = newline + 1;
    }
    if (body == std::string::npos) throw std::runtime_error("missing closing YAML delimiter");
    const auto header = YAML::Load(text.substr(first + 1, end - first - 1));
    if (!header.IsMap()) throw std::runtime_error("frontmatter must be a mapping");
    agent::SkillDefinition skill;
    skill.file = file;
    skill.name = scalar(header, "name", true);
    skill.description = scalar(header, "description", true);
    if (!agent::valid_skill_name(skill.name) || skill.name != file.parent_path().filename().string())
        throw std::runtime_error("name must match the directory and use 1-64 lowercase letters, digits or single hyphens");
    if (skill.description.find_first_not_of(" \t\r\n") == std::string::npos || characters(skill.description) > 1024)
        throw std::runtime_error("description must contain 1-1024 characters");
    skill.body = text.substr(body);
    if (skill.body.find_first_not_of(" \t\r\n") == std::string::npos)
        throw std::runtime_error("skill instructions must not be empty");
    skill.license = scalar(header, "license");
    skill.compatibility = scalar(header, "compatibility");
    if (header["compatibility"] && (skill.compatibility.empty() || characters(skill.compatibility) > 500))
        throw std::runtime_error("compatibility must contain 1-500 characters");
    skill.allowed_tools = scalar(header, "allowed-tools");
    if (const auto metadata = header["metadata"]) {
        if (!metadata.IsMap()) throw std::runtime_error("metadata must be a string mapping");
        for (const auto& item : metadata) {
            if (!item.first.IsScalar() || !item.second.IsScalar())
                throw std::runtime_error("metadata must be a string mapping");
            skill.metadata.emplace(item.first.as<std::string>(), item.second.as<std::string>());
        }
    }
    return skill;
}
} // namespace

std::shared_ptr<const agent::SkillCatalog> load_skills(const fs::path& directory,
                                                     const workspace::FileOptions& files) {
    auto catalog = std::make_shared<agent::SkillCatalog>();
    const auto diagnostic = [&](const fs::path& path, const std::exception& error) {
        catalog->diagnostics.push_back({path.string(), error.what()});
        base::logger("app")->warn("Skill {}: {}", path.string(), error.what());
    };
    try {
        if (!fs::exists(directory)) return catalog;
        const auto root = fs::canonical(directory);
        std::vector<fs::path> candidates;
        for (const auto& entry : fs::directory_iterator(root)) candidates.push_back(entry.path());
        std::ranges::sort(candidates);
        for (const auto& candidate : candidates) {
            try {
                if (!fs::is_directory(candidate) || !fs::exists(candidate / "SKILL.md")) continue;
                const auto file = fs::canonical(candidate / "SKILL.md");
                if (!under(file, root) || file.parent_path() != fs::canonical(candidate))
                    throw std::runtime_error("SKILL.md must stay inside its registered skill directory");
                auto skill = parse(file, files);
                if (catalog->find(skill.name)) throw std::runtime_error("duplicate skill name: " + skill.name);
                catalog->definitions.push_back(std::move(skill));
            } catch (const std::exception& error) { diagnostic(candidate, error); }
        }
        std::ranges::sort(catalog->definitions, {}, &agent::SkillDefinition::name);
    } catch (const std::exception& error) { diagnostic(directory, error); }
    return catalog;
}

} // namespace dagent::app
