#include "app/config_detail.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace dagent::app {
namespace fs = std::filesystem;
namespace {

using detail::fail;
using detail::log_app;

bool blank(std::string_view text) {
    return text.find_first_not_of(" \t\r") == std::string_view::npos;
}

std::string trim(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) return {};
    const std::size_t end = text.find_last_not_of(" \t\r");
    return std::string(text.substr(begin, end - begin + 1));
}

std::vector<std::string> split_list(std::string_view value) {
    std::vector<std::string> items;
    for (std::string_view rest = value;;) {
        const auto comma = rest.find(',');
        std::string item = trim(rest.substr(0, comma));
        if (!item.empty()) items.push_back(std::move(item));
        if (comma == std::string_view::npos) break;
        rest = rest.substr(comma + 1);
    }
    return items;
}

std::vector<std::string> parse_tools(std::string_view value) {
    std::string_view inner = value;
    if (inner.starts_with('[') && inner.ends_with(']')) inner = inner.substr(1, inner.size() - 2);
    return split_list(inner);
}

int parse_int(const fs::path& file, std::size_t line, std::string_view value, std::string_view key) {
    int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result < 0)
        fail(ConfigError::Kind::invalid, std::format("{}:{}: {} must be a non-negative integer", file.filename().string(), line, key));
    return result;
}

agent::SubagentDef parse_subagent(const fs::path& file, const std::string& text,
                                  const std::map<std::string, llm::ProviderConfig>& models) {
    const auto fail_at = [&](ConfigError::Kind kind, std::size_t line, const std::string& what) {
        fail(kind, std::format("{}:{}: {}", file.filename().string(), line, what));
    };
    std::vector<std::string> lines;
    for (std::string_view rest = text;;) {
        const auto newline = rest.find('\n');
        std::string line(rest.substr(0, newline));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (newline == std::string_view::npos) break;
        rest = rest.substr(newline + 1);
    }
    if (lines.empty() || lines.front() != "---")
        fail_at(ConfigError::Kind::parse, 1, "subagent file must start with a '---' frontmatter block");
    std::size_t end = 0;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i] == "---") { end = i; break; }
    }
    if (end == 0) fail_at(ConfigError::Kind::parse, 1, "subagent frontmatter is missing its closing '---'");

    agent::SubagentDef def;
    std::size_t name_line = 1, permission_line = 1, model_line = 1;
    bool tools_open = false;
    for (std::size_t i = 1; i < end; ++i) {
        const std::string& line = lines[i];
        if (blank(line) || line.front() == '#') continue;
        if (line.front() == '-') {
            if (!tools_open) fail_at(ConfigError::Kind::parse, i + 1, "list item outside a tools key");
            std::string item = trim(line.substr(1));
            if (!item.empty()) def.tools->push_back(std::move(item));
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) fail_at(ConfigError::Kind::parse, i + 1, "expected 'key: value'");
        tools_open = false;
        const std::string key = trim(std::string_view(line).substr(0, colon));
        const std::string value = trim(std::string_view(line).substr(colon + 1));
        if (key == "name") { def.name = value; name_line = i + 1; }
        else if (key == "description") def.description = value;
        else if (key == "model") { def.model = value; model_line = i + 1; }
        else if (key == "tools") { def.tools = parse_tools(value); tools_open = true; }
        else if (key == "permission") { def.permission = value; permission_line = i + 1; }
        else if (key == "max_model_calls") def.max_model_calls = parse_int(file, i + 1, value, key);
        else if (key == "max_tool_calls") def.max_tool_calls = parse_int(file, i + 1, value, key);
        else log_app()->warn("{}:{}: unknown subagent key {}", file.filename().string(), i + 1, key);
    }
    if (def.name.empty()) fail_at(ConfigError::Kind::invalid, name_line, "name must not be empty");
    if (def.name.find_first_of(" \t") != std::string::npos || def.name.find("__") != std::string::npos)
        fail_at(ConfigError::Kind::invalid, name_line, "name must not contain whitespace or '__'");
    if (def.permission != "inherit" && def.permission != "read_only" && def.permission != "ask")
        fail_at(ConfigError::Kind::invalid, permission_line, "permission must be inherit, read_only or ask");
    if (!def.model.empty() && !models.contains(def.model))
        fail_at(ConfigError::Kind::invalid, model_line, std::format("model {} is not defined in models.json", def.model));

    std::string body;
    for (std::size_t i = end + 1; i < lines.size(); ++i) {
        body += lines[i];
        if (i + 1 < lines.size()) body += '\n';
    }
    while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back()))) body.pop_back();
    if (body.empty()) fail_at(ConfigError::Kind::invalid, end + 1, "system prompt body must not be empty");
    def.system_prompt = std::move(body);
    return def;
}

} // namespace

std::vector<agent::SubagentDef> load_subagents(
    const fs::path& dir, const std::map<std::string, llm::ProviderConfig>& models) {
    std::vector<agent::SubagentDef> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".md") files.push_back(entry.path());
    }
    if (ec) fail(ConfigError::Kind::io, "cannot list " + dir.string() + ": " + ec.message());
    std::ranges::sort(files); // 工具列表顺序影响前缀缓存，按文件名固定
    std::set<std::string> names;
    for (const fs::path& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) fail(ConfigError::Kind::io, "cannot open subagent file: " + file.string());
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        agent::SubagentDef def = parse_subagent(file, text, models);
        if (!names.insert(def.name).second)
            fail(ConfigError::Kind::invalid, file.filename().string() + ": duplicate subagent name " + def.name);
        out.push_back(std::move(def));
    }
    return out;
}

} // namespace dagent::app
