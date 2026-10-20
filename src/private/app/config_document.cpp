#include "app/config_detail.hpp"

#include <cctype>
#include <fstream>
#include <iterator>

namespace dagent::app::detail {
namespace fs = std::filesystem;

ConfigDocument::ConfigDocument(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) fail(ConfigError::Kind::io, "cannot open config file: " + file.string());
    source.assign(std::istreambuf_iterator<char>(in), {});
    original = value = Ordered::parse(source, nullptr, true, true);
    if (source.find("\r\n") != std::string::npos) newline = "\r\n";
    const auto line = source.find('\n');
    if (line != std::string::npos) {
        const auto end = source.find_first_not_of(" \t", line + 1);
        if (end > line + 1) indent = source.substr(line + 1, end - line - 1);
    }
}

void ConfigDocument::trivia(std::size_t& p) const {
    while (p < source.size()) {
        if (std::isspace(static_cast<unsigned char>(source[p]))) { ++p; continue; }
        if (source.compare(p, 2, "//") == 0) { p = source.find('\n', p + 2); continue; }
        if (source.compare(p, 2, "/*") == 0) { p = source.find("*/", p + 2) + 2; continue; }
        break;
    }
}

std::size_t ConfigDocument::token_end(std::size_t p) const {
    if (source[p] == '"') {
        for (++p; source[p] != '"'; ++p) if (source[p] == '\\') ++p;
        return p + 1;
    }
    if (source[p] == '{' || source[p] == '[') {
        const char end = source[p++] == '{' ? '}' : ']';
        trivia(p);
        while (source[p] != end) {
            if (source[p] == ',' || source[p] == ':') ++p;
            else p = token_end(p);
            trivia(p);
        }
        return p + 1;
    }
    while (p < source.size() && !std::isspace(static_cast<unsigned char>(source[p])) &&
           source[p] != ',' && source[p] != ']' && source[p] != '}' && source[p] != '/') ++p;
    return p;
}

std::string ConfigDocument::encoded(const Ordered& item, const std::string& padding, bool multiline) const {
    if (item.is_array() && !multiline) {
        std::string out = "[";
        for (const auto& value : item) {
            if (out.size() > 1) out += ", ";
            out += encoded(value, padding, false);
        }
        return out + "]";
    }
    const auto raw = item.dump(multiline ? static_cast<int>(indent.size()) : -1, indent.front());
    std::string out;
    for (char c : raw) {
        if (c == '\n') out += newline + padding;
        else out += c;
    }
    return out;
}

std::string ConfigDocument::patch(const Ordered& before, const Ordered& after, std::size_t begin, std::size_t end) const {
    if (before == after) return source.substr(begin, end - begin);
    const auto line = source.rfind('\n', begin);
    const auto start = line == std::string::npos ? 0 : line + 1;
    const auto first = source.find_first_not_of(" \t", start);
    const auto padding = source.substr(start, first - start);
    const bool multiline = source.substr(begin, end - begin).find('\n') != std::string::npos;
    if (!before.is_object() || !after.is_object()) return encoded(after, padding, multiline);
    std::string out = "{";
    std::size_t p = begin + 1, closing = p;
    bool any = false;
    const auto append = [&](const std::string& entry) {
        if (any) out += ',';
        out += entry; any = true;
    };
    trivia(p);
    while (source[p] != '}') {
        const auto key_start = p, key_end = token_end(p);
        const auto key = Ordered::parse(source.substr(key_start, key_end - key_start)).get<std::string>();
        p = key_end; trivia(p); ++p; trivia(p);
        const auto value_start = p, value_end = token_end(p);
        p = value_end; trivia(p);
        if (after.contains(key)) {
            auto entry = source.substr(closing, value_start - closing) + patch(before.at(key), after.at(key), value_start, value_end);
            if (source[p] == ',') entry += source.substr(value_end, p - value_end);
            append(entry);
        }
        closing = value_end;
        if (source[p] == ',') { closing = ++p; trivia(p); }
    }
    const auto tail = source.substr(closing, end - closing - 1);
    for (auto it = after.begin(); it != after.end(); ++it) {
        if (before.contains(it.key())) continue;
        const auto child_padding = padding + indent;
        const auto prefix = multiline ? newline + child_padding : any ? " " : "";
        append(prefix + Ordered(it.key()).dump() + ": " + encoded(it.value(), child_padding, multiline));
    }
    return out + tail + "}";
}

std::string ConfigDocument::dump() const {
    std::size_t begin = 0; trivia(begin);
    const auto end = token_end(begin);
    return source.substr(0, begin) + patch(original, value, begin, end) + source.substr(end);
}

} // namespace dagent::app::detail
