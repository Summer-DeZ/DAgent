#include "base/json.hpp"

#include <cctype>
#include <cstddef>
#include <string_view>

namespace dagent::base {
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
