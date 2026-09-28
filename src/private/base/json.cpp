#include "base/json.hpp"

#include <charconv>
#include <format>
#include <limits>
#include <system_error>
#include <cctype>
#include <cstddef>
#include <string_view>

namespace dagent::base {
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

namespace {

char ascii_lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool name_matches(std::string_view key, std::span<const std::string> fields) {
    for (const auto& field : fields) {
        if (key.size() != field.size()) continue;
        std::size_t i = 0;
        while (i < key.size() && ascii_lower(key[i]) == ascii_lower(field[i])) ++i;
        if (i == key.size()) return true;
    }
    return false;
}

} // namespace

void redact(nlohmann::json& j, std::span<const std::string> fields) {
    if (j.is_object()) {
        for (auto& [key, value] : j.items()) {
            if (name_matches(key, fields)) value = "***";
            else redact(value, fields);
        }
    } else if (j.is_array()) {
        for (auto& value : j) redact(value, fields);
    }
}

} // namespace dagent::base
