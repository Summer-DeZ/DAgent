#include "mcp/detail.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <set>

namespace dagent::mcp::detail {
namespace {

using nlohmann::json;

bool is_tchar(unsigned char c) {
    return std::isalnum(c) != 0 || c == '!' || c == '#' || c == '$' || c == '%' || c == '&' ||
           c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' || c == '^' || c == '_' ||
           c == '`' || c == '|' || c == '~';
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string base64_encode(std::string_view in) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const unsigned b0 = static_cast<unsigned char>(in[i]);
        const unsigned b1 = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
        const unsigned b2 = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
        out += kTable[b0 >> 2];
        out += kTable[((b0 & 0x03) << 4) | (b1 >> 4)];
        out += i + 1 < in.size() ? kTable[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=';
        out += i + 2 < in.size() ? kTable[b2 & 0x3f] : '=';
    }
    return out;
}

bool is_data_key(std::string_view key) {
    return key == "enum" || key == "default" || key == "examples" || key == "const";
}

// 找出不在 properties 链上的 x-mcp-header 注解（items / oneOf / $defs / 根节点等处）。
// properties 映射里的键是参数名、enum/default 等关键字里的是数据，都不算注解。
bool find_stray_annotation(const json& node, bool allowed) {
    if (node.is_array()) {
        for (const auto& item : node)
            if (find_stray_annotation(item, false)) return true;
        return false;
    }
    if (!node.is_object()) return false;
    if (node.contains("x-mcp-header") && !allowed) return true;
    for (const auto& [key, value] : node.items()) {
        if (key == "x-mcp-header" || is_data_key(key)) continue;
        if (key == "properties") {
            if (!value.is_object()) continue;
            for (const auto& [name, sub] : value.items()) {
                (void)name;
                // 已经在链上时，嵌套 properties 仍在链上；否则整棵子树都不在。
                if (find_stray_annotation(sub, allowed)) return true;
            }
            continue;
        }
        if (find_stray_annotation(value, false)) return true;
    }
    return false;
}

bool contains_stray_annotation(const json& schema) {
    if (!schema.is_object()) return false;
    if (schema.contains("x-mcp-header")) return true; // 根上的注解没有参数路径
    for (const auto& [key, value] : schema.items()) {
        if (key == "x-mcp-header" || is_data_key(key)) continue;
        if (key == "properties") {
            if (!value.is_object()) continue;
            for (const auto& [name, sub] : value.items()) {
                (void)name;
                if (find_stray_annotation(sub, true)) return true;
            }
            continue;
        }
        if (find_stray_annotation(value, false)) return true;
    }
    return false;
}

// 沿 properties 链递归收集注解；items / oneOf 等结构到不了这里，事后按总数量对不上就整条剔除。
bool walk_properties(const json& schema, std::vector<std::string>& path, std::vector<HeaderParam>& out,
                     std::set<std::string>& seen, std::string& reason) {
    const auto props = schema.find("properties");
    if (props == schema.end() || !props->is_object()) return true;
    for (const auto& [name, prop] : props->items()) {
        if (!prop.is_object()) continue;
        path.push_back(name);
        if (const auto annotation = prop.find("x-mcp-header"); annotation != prop.end()) {
            if (!annotation->is_string() || annotation->get<std::string>().empty()) {
                reason = "参数 " + name + " 的 x-mcp-header 必须是非空字符串";
                return false;
            }
            const std::string header = annotation->get<std::string>();
            if (!std::ranges::all_of(header, [](unsigned char c) { return is_tchar(c); })) {
                reason = "参数 " + name + " 的 x-mcp-header 不是合法的 HTTP 头名字";
                return false;
            }
            if (!seen.insert(lower(header)).second) {
                reason = "x-mcp-header 重名：" + header;
                return false;
            }
            const auto type = prop.find("type");
            if (type == prop.end() || !type->is_string() ||
                (*type != "string" && *type != "integer" && *type != "boolean")) {
                reason = "参数 " + name + " 带 x-mcp-header，类型只能是 string/integer/boolean";
                return false;
            }
            out.push_back({header, path});
        }
        if (!walk_properties(prop, path, out, seen, reason)) return false;
        path.pop_back();
    }
    return true;
}

const json* dig(const json& root, const std::vector<std::string>& path) {
    const json* cur = &root;
    for (const auto& key : path) {
        if (!cur->is_object()) return nullptr;
        const auto it = cur->find(key);
        if (it == cur->end()) return nullptr;
        cur = &*it;
    }
    return cur;
}

} // namespace

bool is_known_legacy_version(std::string_view v) {
    static constexpr std::string_view kVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26",
                                                     "2024-11-05"};
    return std::ranges::find(kVersions, v) != std::end(kVersions);
}

std::string pick_modern_version(const json& supported) {
    if (!supported.is_array()) return {};
    for (const auto& v : supported)
        if (v.is_string() && v.get<std::string>() == kModernVersion) return std::string(kModernVersion);
    return {};
}

json modern_meta() {
    return {{"io.modelcontextprotocol/protocolVersion", std::string(kModernVersion)},
            {"io.modelcontextprotocol/clientInfo", client_info()},
            {"io.modelcontextprotocol/clientCapabilities", json::object()}};
}

json client_info() {
    return {{"name", "DAgent"}, {"version", DAGENT_VERSION}};
}

json make_request(int64_t id, std::string_view method, json params) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", std::string(method)}, {"params", std::move(params)}};
}

json make_notification(std::string_view method, json params) {
    return {{"jsonrpc", "2.0"}, {"method", std::string(method)}, {"params", std::move(params)}};
}

json make_response(const json& id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json make_error_response(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

std::string sanitize(std::string_view s) { return sanitize_name(s); }

std::string qualified_name(std::string_view server, std::string_view tool) {
    return "mcp__" + sanitize(server) + "__" + sanitize(tool);
}

bool collect_header_params(const json& schema, std::vector<HeaderParam>& out, std::string& reason) {
    std::set<std::string> seen;
    std::vector<std::string> path;
    if (!walk_properties(schema, path, out, seen, reason)) return false;
    if (contains_stray_annotation(schema)) {
        reason = "x-mcp-header 只能出现在 properties 链上的原始类型参数上";
        return false;
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> header_param_values(const std::vector<HeaderParam>& params,
                                                                     const json& arguments) {
    constexpr std::int64_t kMaxSafeInteger = 9007199254740991;
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& param : params) {
        const json* value = dig(arguments, param.path);
        if (value == nullptr || value->is_null()) continue;
        std::string text;
        if (value->is_string()) {
            text = value->get<std::string>();
        } else if (value->is_boolean()) {
            text = value->get<bool>() ? "true" : "false";
        } else if (value->is_number_unsigned()) {
            const std::uint64_t n = value->get<std::uint64_t>();
            if (n > static_cast<std::uint64_t>(kMaxSafeInteger)) continue;
            text = std::to_string(n);
        } else if (value->is_number_integer()) {
            const std::int64_t n = value->get<std::int64_t>();
            if (n < -kMaxSafeInteger || n > kMaxSafeInteger) continue;
            text = std::to_string(n);
        } else if (value->is_number_float()) {
            // schema 声明的是 integer，但 JSON 里可能是 42.0；能精确转成整数才发头。
            const double number = value->get<double>();
            if (!std::isfinite(number) || std::floor(number) != number ||
                number < -static_cast<double>(kMaxSafeInteger) ||
                number > static_cast<double>(kMaxSafeInteger))
                continue;
            text = std::to_string(static_cast<std::int64_t>(number));
        } else {
            continue; // 类型不符由 server 校验请求体时报错，这里不产生头
        }
        out.emplace_back("Mcp-Param-" + param.header, encode_header_value(text));
    }
    return out;
}

std::string encode_header_value(std::string_view value) {
    bool plain = !value.empty();
    if (value.starts_with("=?base64?") && value.ends_with("?=")) plain = false;
    for (std::size_t i = 0; i < value.size() && plain; ++i) {
        const auto uc = static_cast<unsigned char>(value[i]);
        const bool whitespace = uc == ' ' || uc == '\t';
        if (whitespace) {
            if (i == 0 || i + 1 == value.size()) plain = false; // 首尾空白要编码
        } else if (uc < 0x21 || uc > 0x7E) {
            plain = false;
        }
    }
    if (!plain) return "=?base64?" + base64_encode(value) + "?=";
    return std::string(value);
}

} // namespace dagent::mcp::detail

namespace dagent::mcp {

std::string sanitize_name(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) == 0 && c != '_' && c != '-') c = '_';
    }
    return out;
}

} // namespace dagent::mcp
