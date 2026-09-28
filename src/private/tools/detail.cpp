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
