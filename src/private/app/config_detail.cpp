#include "app/config_detail.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>

#include "base/path.hpp"

namespace dagent::app::detail {
namespace fs = std::filesystem;

std::shared_ptr<spdlog::logger> log_app() { return base::logger("app"); }

[[noreturn]] void fail(ConfigError::Kind kind, const std::string& what) {
    throw ConfigError(kind, what);
}

fs::path absolute_under(const fs::path& base, const fs::path& path) {
    return path.is_absolute() ? base::canonical_or_absolute_or_lexical(path)
                              : base::canonical_or_absolute_or_lexical(base / path);
}

std::size_t size_option(const Node& node, std::string_view key) {
    const Node value = node.required(key);
    const std::size_t size = value.usize();
    if (size == 0) fail(ConfigError::Kind::invalid, value.pointer() + " must be greater than zero");
    return size;
}

json parse_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) fail(ConfigError::Kind::io, "cannot open config file: " + file.string());
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    try {
        json value = json::parse(text, nullptr, true, true);
        if (!value.is_object()) fail(ConfigError::Kind::parse, file.string() + ": top level must be a JSON object");
        return value;
    } catch (const json::parse_error& e) {
        fail(ConfigError::Kind::parse, file.string() + ": " + e.what());
    }
}

std::string env_value(std::string_view name, const std::string& where) {
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr || *value == '\0')
        fail(ConfigError::Kind::invalid, std::format("{} references unset environment variable {}", where, name));
    return value;
}

std::string expand_env(const std::string& text, const std::string& where) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '{') {
            const auto close = text.find('}', i + 2);
            if (close != std::string::npos) {
                out += env_value(std::string_view(text).substr(i + 2, close - i - 2), where);
                i = close + 1;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

} // namespace dagent::app::detail
