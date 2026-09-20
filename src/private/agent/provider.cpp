#include "agent/provider_detail.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dagent::agent::provider_detail {
using nlohmann::json;

std::string join_url(std::string_view base, std::string_view path) {
    std::string url(base);
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url + std::string(path);
}

std::string string_field(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

int int_field(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<int>() : 0;
}

std::string snippet(std::string_view text, std::size_t limit) {
    std::string out;
    out.reserve(std::min(text.size(), limit));
    for (const char c : text) {
        if (out.size() >= limit) break;
        out += (c == '\n' || c == '\r') ? ' ' : c;
    }
    if (text.size() > limit) out += "…";
    return out;
}

// 错误体里常见 {"error":{...}}，也可能是别的形状；只取人类可读的部分。
std::string error_message(const json& error) {
    if (error.is_string()) return error.get<std::string>();
    if (error.is_object()) {
        std::string message = string_field(error, "message");
        const std::string code = string_field(error, "code");
        if (!code.empty() && code != message) {
            message += message.empty() ? code : std::format(" [{}]", code);
        }
        return message;
    }
    return {};
}

std::optional<std::chrono::milliseconds> parse_retry_after(std::string_view raw) {
    const std::size_t begin = raw.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return std::nullopt;
    const std::size_t end = raw.find_last_not_of(" \t\r\n");
    const std::string value(raw.substr(begin, end - begin + 1));

    if (std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        if (value.size() > 12) return std::nullopt;
        return std::chrono::seconds(std::stoll(value));
    }

    // HTTP-date：Wed, 21 Oct 2015 07:28:00 GMT（不依赖 locale，手工映射月份）
    static constexpr std::string_view months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char weekday[4] = {};
    char month[4] = {};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(value.c_str(), "%3s, %2d %3s %4d %2d:%2d:%2d", weekday, &day, month, &year, &hour,
                    &minute, &second) != 7) {
        return std::nullopt;
    }
    const auto found = std::find_if(std::begin(months), std::end(months),
                                    [&](std::string_view name) { return name == month; });
    if (found == std::end(months) || year < 1970) return std::nullopt;

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = static_cast<int>(found - std::begin(months));
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    const std::time_t when = ::timegm(&tm);
    const auto delta = std::chrono::system_clock::from_time_t(when) - std::chrono::system_clock::now();
    if (delta <= std::chrono::system_clock::duration::zero()) return std::chrono::milliseconds(0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(delta);
}

bool looks_like_context_overflow(std::string_view detail) {
    std::string lower(detail);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static constexpr std::string_view needles[] = {
        "prompt is too long", "input length exceeds", "context length", "context_length", "maximum context", "max context",
        "too many tokens", "token limit",    "exceeds the maximum", "reduce the length",
        "exceeds the available context size", // llama-server 的超长请求
    };
    return std::any_of(std::begin(needles), std::end(needles),
                       [&](std::string_view needle) { return lower.find(needle) != std::string::npos; });
}

std::string_view role_name(Role role) {
    switch (role) {
        case Role::system: return "system";
        case Role::user: return "user";
        case Role::assistant: return "assistant";
        case Role::tool: return "tool";
    }
    return "user";
}

json encode_tools(const std::vector<ToolDef>& tools) {
    json out = json::array();
    for (const ToolDef& tool : tools) {
        out.push_back({{"type", "function"},
                       {"function",
                        {{"name", tool.name},
                         {"description", tool.description},
                         {"parameters", tool.parameters}}}});
    }
    return out;
}


Error classify_http(const net::HttpResponse& response, std::string_view api_key) {
        Error error;
        std::string detail;
        if (const json parsed = json::parse(response.body, nullptr, false); !parsed.is_discarded()) {
            if (const auto it = parsed.find("error"); it != parsed.end()) detail = error_message(*it);
        }
        if (detail.empty()) detail = snippet(response.body, 300);
        error.message = std::format("HTTP {}: {}", response.status, detail.empty() ? "no detail" : detail);

        switch (response.status) {
            case 408:
            case 429:
                error.retryable = true;
                break;
            default:
                error.retryable = response.status >= 500;
                break;
        }
        if (error.retryable) {
            if (const auto header = response.header("retry-after")) {
                if (const auto delay = parse_retry_after(*header)) error.retry_after = *delay;
            }
        }
        if (response.status == 400 && looks_like_context_overflow(detail)) error.context_too_long = true;
        if (!api_key.empty()) {
            for (std::size_t pos = 0; (pos = error.message.find(api_key, pos)) != std::string::npos;) {
                error.message.replace(pos, api_key.size(), "[redacted]");
                pos += 10;
            }
        }
        return error;
}

void merge_extra(json& body, const json& extra) {
    for (auto it = extra.begin(); it != extra.end(); ++it)
        if (!body.contains(it.key())) body[it.key()] = it.value();
}
} // namespace dagent::agent::provider_detail

namespace dagent::agent {
std::span<const ProviderInfo> providers() noexcept {
    static constexpr ProviderInfo entries[] = {
        {"openai-chat", "https://api.openai.com/v1", Framing::sse, false, false},
        {"anthropic", "https://api.anthropic.com/v1", Framing::sse, true, true},
        {"ollama", "http://127.0.0.1:11434", Framing::ndjson, false, false},
    };
    return entries;
}
const ProviderInfo* find_provider(std::string_view kind) noexcept {
    for (const auto& info : providers()) if (info.kind == kind) return &info;
    return nullptr;
}
std::unique_ptr<Codec> make_codec(const ProviderConfig& config) {
    const auto* info = find_provider(config.kind);
    if (!info) throw std::invalid_argument("unknown provider: " + config.kind);
    auto resolved = config;
    if (resolved.base_url.empty()) resolved.base_url = info->default_base_url;
    if (resolved.kind == "openai-chat") return provider_detail::make_chat(resolved);
    if (resolved.kind == "anthropic") return provider_detail::make_anthropic(resolved);
    return provider_detail::make_ollama(resolved);
}
} // namespace dagent::agent
