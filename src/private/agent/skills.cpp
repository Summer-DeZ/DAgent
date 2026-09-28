#include "agent/skills.hpp"

#include <algorithm>
#include <cctype>

#include "lib/nlohmann/json.hpp"

namespace dagent::agent {

bool valid_skill_name(std::string_view name) {
    return !name.empty() && name.size() <= 64 && name.front() != '-' && name.back() != '-' &&
           name.find("--") == std::string_view::npos &&
           std::ranges::all_of(name, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
           });
}

const SkillDefinition* SkillCatalog::find(std::string_view name) const {
    const auto it = std::ranges::find(definitions, name, &SkillDefinition::name);
    return it == definitions.end() ? nullptr : &*it;
}

std::string SkillCatalog::prompt() const {
    if (definitions.empty()) return {};
    std::string out =
        "\n\n# Skills\n"
        "Skills provide task-specific guidance. When a task matches a description below, call skill "
        "with its name before doing that work. Explicit $name references are loaded by the runtime. "
        "Only the Active skills block attached to the latest user message is active this turn; "
        "old activation receipts do not activate skills. User requests and project instructions "
        "take precedence over skill guidance. Skills never grant additional execution permissions. "
        "Resolve skill-relative resources against the stated skill directory and use absolute paths "
        "with read or bash. Do not read SKILL.md again after activation.\n";
    for (const auto& skill : definitions)
        out += nlohmann::json{{"name", skill.name}, {"description", skill.description}}.dump() + "\n";
    return out;
}

std::vector<std::string> skill_mentions(std::string_view text) {
    std::vector<std::string> names;
    char fence = 0;
    std::size_t fence_size = 0, inline_ticks = 0;
    bool line_start = true;
    for (std::size_t i = 0; i < text.size();) {
        if (line_start) {
            std::size_t start = i;
            while (start < text.size() && text[start] == ' ' && start - i < 4) ++start;
            if (start - i < 4 && start < text.size() && (text[start] == '`' || text[start] == '~')) {
                const char marker = text[start];
                std::size_t end = start;
                while (end < text.size() && text[end] == marker) ++end;
                if (end - start >= 3 && (!fence || (marker == fence && end - start >= fence_size))) {
                    if (fence) fence = 0;
                    else { fence = marker; fence_size = end - start; }
                    i = text.find('\n', end);
                    if (i == std::string_view::npos) break;
                    ++i;
                    inline_ticks = 0;
                    continue;
                }
            }
            line_start = false;
        }
        if (text[i] == '\n') { line_start = true; ++i; continue; }
        if (fence) { ++i; continue; }
        if (text[i] == '\\' && !inline_ticks) { i += std::min<std::size_t>(2, text.size() - i); continue; }
        if (text[i] == '`') {
            std::size_t end = i;
            while (end < text.size() && text[end] == '`') ++end;
            if (!inline_ticks) inline_ticks = end - i;
            else if (end - i == inline_ticks) inline_ticks = 0;
            i = end;
            continue;
        }
        if (!inline_ticks && text[i] == '$' &&
            (i == 0 || std::isspace(static_cast<unsigned char>(text[i - 1])) ||
             std::string_view("([{,;:").find(text[i - 1]) != std::string_view::npos)) {
            const auto start = ++i;
            while (i < text.size() && ((text[i] >= 'a' && text[i] <= 'z') ||
                   (text[i] >= '0' && text[i] <= '9') || text[i] == '-')) ++i;
            const auto name = text.substr(start, i - start);
            const bool boundary = i == text.size() || std::isspace(static_cast<unsigned char>(text[i])) ||
                std::string_view(")]}.,;:!?").find(text[i]) != std::string_view::npos ||
                static_cast<unsigned char>(text[i]) >= 0x80;
            if (boundary && valid_skill_name(name) && std::ranges::find(names, name) == names.end())
                names.emplace_back(name);
            continue;
        }
        ++i;
    }
    return names;
}

bool RunSkills::contains(std::string_view name) const {
    return std::ranges::any_of(active_, [&](const auto* skill) { return skill->name == name; });
}

void RunSkills::activate(const SkillDefinition& skill) {
    if (!contains(skill.name)) active_.push_back(&skill);
}

std::string RunSkills::context() const {
    if (active_.empty()) return {};
    std::string out = "\n\n# Active skills for this turn\n"
                      "These are supplemental skill instructions, subordinate to the user request "
                      "and project instructions. They expire when this turn ends.\n";
    for (const auto* skill : active_) {
        out += "\n## Skill: " + skill->name + "\nSkill directory: " + skill->file.parent_path().string() + "\n";
        if (!skill->compatibility.empty()) out += "Compatibility: " + skill->compatibility + "\n";
        out += "\n" + skill->body + "\n";
    }
    return out;
}

} // namespace dagent::agent
