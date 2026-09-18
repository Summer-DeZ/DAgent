#include "base/dotenv.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace dagent::base {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::string unescape_double_quoted(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == 'n') {
            out += '\n';
            ++i;
        } else {
            out += value[i];
        }
    }
    return out;
}

// 不带引号的值在「空白 + #」处结束；a#b 里的 # 属于值本身。
std::string_view cut_inline_comment(std::string_view raw) {
    for (std::size_t i = 1; i < raw.size(); ++i) {
        if (raw[i] == '#' && (raw[i - 1] == ' ' || raw[i - 1] == '\t'))
            return raw.substr(0, i);
    }
    return raw;
}

} // namespace

std::vector<std::pair<std::string, std::string>> parse_dotenv(std::string_view text) {
    std::vector<std::pair<std::string, std::string>> entries;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto newline = text.find('\n', pos);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        std::string_view line = trim(text.substr(pos, end - pos));
        pos = end + 1;

        if (line.empty() || line.front() == '#') continue;
        if (line.starts_with("export ")) {
            line.remove_prefix(7);
            line = trim(line);
        }

        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, eq));
        if (key.empty()) continue;
        // 引号判断看去掉左侧空白后的值（KEY = "v"）；行内注释仍按原始值切，
        // 这样 "KEY= # 注释" 里 # 前的空白还在，能识别为空值。
        const std::string_view raw = line.substr(eq + 1);
        std::string_view value = raw;
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);

        std::string decoded;
        if (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
            const char quote = value.front();
            if (const auto close = value.find(quote, 1); close != std::string_view::npos) {
                const std::string_view content = value.substr(1, close - 1);
                decoded = quote == '"' ? unescape_double_quoted(content) : std::string(content);
            } else { // 引号不配对：按普通值处理
                decoded = trim(cut_inline_comment(raw));
            }
        } else {
            decoded = trim(cut_inline_comment(raw));
        }
        entries.emplace_back(std::string(key), std::move(decoded));
    }
    return entries;
}

Secrets Secrets::load(std::span<const std::filesystem::path> files) {
    Secrets secrets;
    for (const auto& file : files) {
        if (!std::filesystem::exists(file)) continue;
        std::ifstream in(file, std::ios::binary);
        if (!in) throw std::runtime_error("cannot read env file: " + file.string());
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (auto& [key, value] : parse_dotenv(text))
            secrets.values_.try_emplace(std::move(key), std::move(value));
    }
    return secrets;
}

std::optional<std::string> Secrets::get(std::string_view name) const {
    if (const char* env = std::getenv(std::string(name).c_str())) return std::string(env);
    if (auto it = values_.find(std::string(name)); it != values_.end()) return it->second;
    return std::nullopt;
}

} // namespace dagent::base
