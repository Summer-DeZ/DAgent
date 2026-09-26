#include "tools/detail.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <limits>
#include <system_error>

#include "base/log.hpp"
#include "base/text.hpp"

namespace dagent::tools::detail {
namespace fs = std::filesystem;
namespace {

using nlohmann::json;

std::string where(std::string_view key) { return std::format("argument {}", key); }

/// 字符串字段收到数字时的宽容处理（模型偶尔把路径写成数字的情况不管，只接常见形态）。
std::optional<std::string> string_from(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return value.dump();
    return std::nullopt;
}

} // namespace

std::expected<json, std::string> parse_arguments(std::string_view arguments) {
    const std::string_view trimmed = [] (std::string_view s) {
        const auto is_space = [](unsigned char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        };
        while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
        while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
        return s;
    }(arguments);
    if (trimmed.empty()) return json::object();
    json parsed;
    try {
        parsed = json::parse(trimmed);
    } catch (const json::parse_error& e) {
        return std::unexpected(std::format("arguments are not valid JSON: {}", e.what()));
    }
    if (!parsed.is_object()) return std::unexpected("arguments must be a JSON object");
    return parsed;
}

std::string require_string(const json& args, std::string_view key, std::string& err) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        err = std::format("{} is required", where(key));
        return {};
    }
    if (const auto text = string_from(*it)) {
        if (text->empty()) {
            err = std::format("{} must not be empty", where(key));
            return {};
        }
        return *text;
    }
    err = std::format("{} must be a string", where(key));
    return {};
}

std::optional<std::string> get_string(const json& args, std::string_view key, std::string& err) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) return std::nullopt;
    if (const auto text = string_from(*it)) return *text;
    err = std::format("{} must be a string", where(key));
    return std::nullopt;
}

std::optional<std::int64_t> get_int(const json& args, std::string_view key, std::string& err) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) return std::nullopt;
    if (it->is_number_integer() && !it->is_number_unsigned())
        return it->get<std::int64_t>();
    if (it->is_number_unsigned()) {
        const auto value = it->get<std::uint64_t>();
        if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return static_cast<std::int64_t>(value);
    }
    if (it->is_string()) {
        const std::string text = it->get<std::string>();
        std::int64_t value = 0;
        const char* first = text.data();
        const char* last = first + text.size();
        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec == std::errc{} && ptr == last) return value;
    }
    err = std::format("{} must be an integer", where(key));
    return std::nullopt;
}

std::optional<bool> get_bool(const json& args, std::string_view key, std::string& err) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) return std::nullopt;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) {
        const std::string text = it->get<std::string>();
        if (text == "true") return true;
        if (text == "false") return false;
    }
    err = std::format("{} must be a boolean", where(key));
    return std::nullopt;
}

Result error_result(std::string text, agent::View display) {
    Result result;
    result.model_text = std::move(text);
    result.is_error = true;
    result.display = std::move(display);
    return result;
}

std::string expand_home(std::string_view raw) {
    if (!raw.starts_with("~/") && raw != "~") return std::string(raw);
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return std::string(raw);
    return raw == "~" ? home : std::format("{}/{}", home, raw.substr(2));
}

workspace::Resolved resolve_arg(const Context& ctx, std::string_view raw) {
    return workspace::resolve(ctx.root(), expand_home(raw));
}

std::string display_path(const Context& ctx, const workspace::Resolved& resolved) {
    std::error_code ec;
    const auto relative = fs::relative(resolved.path, ctx.root(), ec);
    if (!ec) {
        std::string text = relative.string();
        if (!text.empty() && text != ".") return text;
    }
    return resolved.path.string();
}

std::string relative_prefix(const fs::path& dir, const fs::path& base) {
    std::error_code ec;
    const fs::path relative = fs::relative(dir, base, ec);
    if (ec) return dir.string() + "/";
    std::string text = relative.string();
    if (text.empty() || text == ".") return {};
    if (!text.ends_with('/')) text += '/';
    return text;
}

// ---------------------------------------------------------------- 文本组装

std::string to_lf(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\r') {
            out += text[i];
            continue;
        }
        out += '\n';
        if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
    }
    return out;
}

std::vector<std::string_view> split_lines(std::string_view content) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= content.size()) {
        if (start == content.size()) break;
        const std::size_t pos = content.find('\n', start);
        if (pos == std::string_view::npos) {
            lines.push_back(content.substr(start));
            break;
        }
        lines.push_back(content.substr(start, pos - start));
        start = pos + 1;
    }
    return lines;
}

std::size_t count_lines(std::string_view content) {
    if (content.empty()) return 0;
    std::size_t lines = 1;
    for (const char c : content)
        if (c == '\n') ++lines;
    if (content.back() == '\n') --lines; // 结尾换行不产生空行
    return lines;
}

std::string fit_line(std::string_view line, std::size_t max_bytes) {
    if (line.size() <= max_bytes) return std::string(line);
    return std::format("{}…", std::string_view(line).substr(0, base::utf8_floor(line, max_bytes)));
}

std::string line_prefix(std::size_t number) { return std::format("{}\t", number); }

std::vector<std::size_t> find_all(std::string_view content, std::string_view needle) {
    std::vector<std::size_t> positions;
    if (needle.empty()) return positions;
    for (std::size_t pos = content.find(needle); pos != std::string_view::npos;
         pos = content.find(needle, pos + needle.size()))
        positions.push_back(pos);
    return positions;
}

std::size_t line_at(std::string_view content, std::size_t position) {
    std::size_t line = 1;
    for (std::size_t i = 0; i < position && i < content.size(); ++i)
        if (content[i] == '\n') ++line;
    return line;
}

} // namespace dagent::tools::detail
