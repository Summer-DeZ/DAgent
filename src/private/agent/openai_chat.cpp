#include "agent/openai_chat.hpp"

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

namespace dagent::agent {
namespace {

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
        "context length", "context_length", "maximum context", "max context",
        "too many tokens", "token limit",    "exceeds the maximum", "reduce the length",
    };
    return std::any_of(std::begin(needles), std::end(needles),
                       [&](std::string_view needle) { return lower.find(needle) != std::string::npos; });
}

Usage parse_usage(const json& usage) {
    Usage out;
    out.prompt = int_field(usage, "prompt_tokens");
    out.completion = int_field(usage, "completion_tokens");
    out.cached = int_field(usage, "prompt_cache_hit_tokens"); // DeepSeek
    if (out.cached == 0) {
        if (const auto details = usage.find("prompt_tokens_details");
            details != usage.end() && details->is_object()) {
            out.cached = int_field(*details, "cached_tokens"); // OpenAI
        }
    }
    return out;
}

Finish::Reason finish_reason_from(std::string_view raw) {
    if (raw == "stop") return Finish::Reason::stop;
    if (raw == "length") return Finish::Reason::length;
    if (raw == "tool_calls" || raw == "function_call") return Finish::Reason::tool_calls;
    if (raw == "content_filter") return Finish::Reason::content_filter;
    // 缺失或未知都不能当成正常结束，否则核心会把截断误当完整回复
    return Finish::Reason::error;
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

json encode_messages(const std::vector<Message>& messages, bool send_reasoning) {
    json out = json::array();
    for (const Message& message : messages) {
        json encoded = json::object();
        encoded["role"] = role_name(message.role);
        if (message.role == Role::tool) {
            encoded["tool_call_id"] = message.tool_call_id;
            encoded["content"] = message.content;
            out.push_back(std::move(encoded));
            continue;
        }
        if (message.tool_calls.empty()) {
            encoded["content"] = message.content;
        } else {
            encoded["content"] = message.content.empty() ? json(nullptr) : json(message.content);
            json calls = json::array();
            for (const ToolCall& call : message.tool_calls) {
                calls.push_back({{"id", call.id},
                                 {"type", "function"},
                                 {"function", {{"name", call.name}, {"arguments", call.arguments}}}});
            }
            encoded["tool_calls"] = std::move(calls);
        }
        if (send_reasoning && !message.reasoning_content.empty()) {
            encoded["reasoning_content"] = message.reasoning_content;
        }
        out.push_back(std::move(encoded));
    }
    return out;
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

class OpenAiChatCodec final : public Codec {
public:
    explicit OpenAiChatCodec(OpenAiChatOptions options) : opt_(std::move(options)) {}

    net::HttpRequest encode(const Request& request) const override {
        json body = json::object();
        body["model"] = request.model;
        body["messages"] = encode_messages(request.messages, opt_.send_reasoning_content);
        body["stream"] = request.stream;
        if (request.stream && opt_.include_usage) {
            body["stream_options"] = {{"include_usage", true}};
        }
        if (request.max_tokens > 0) body["max_tokens"] = request.max_tokens;
        if (request.temperature >= 0.0) body["temperature"] = request.temperature;
        if (!request.tools.empty()) body["tools"] = encode_tools(request.tools);
        for (auto it = opt_.extra_body.begin(); it != opt_.extra_body.end(); ++it) {
            if (!body.contains(it.key())) body[it.key()] = it.value();
        }

        net::HttpRequest http;
        http.method = "POST";
        http.url = join_url(opt_.base_url, "/chat/completions");
        http.headers.push_back({"Content-Type", "application/json"});
        http.headers.push_back({"Accept", request.stream ? "text/event-stream" : "application/json"});
        if (!opt_.api_key.empty()) http.headers.push_back({"Authorization", "Bearer " + opt_.api_key});
        http.body = body.dump();
        return http;
    }

    void decode(const net::SseEvent& event, std::vector<StreamEvent>& out) override {
        if (closed_) return; // 一次请求一个实例；[DONE] 之后的数据一律忽略
        const std::string_view data = event.data;
        if (data == "[DONE]") {
            close(out);
            return;
        }

        const json chunk = json::parse(data, nullptr, false);
        if (chunk.is_discarded()) {
            fail(out, std::format("invalid JSON in SSE data: {}", snippet(data, 120)));
            return;
        }
        if (const auto error = chunk.find("error"); error != chunk.end() && !error->is_null()) {
            fail(out, error_message(*error));
            return;
        }
        if (const auto usage = chunk.find("usage"); usage != chunk.end() && usage->is_object()) {
            usage_ = parse_usage(*usage);
        }

        const auto choices = chunk.find("choices");
        if (choices == chunk.end() || !choices->is_array() || choices->empty()) return; // usage-only chunk
        const json& choice = choices->front();
        if (!choice.is_object()) return;
        if (const auto reason = choice.find("finish_reason"); reason != choice.end() && reason->is_string()) {
            finish_reason_ = reason->get<std::string>();
        }

        const auto delta = choice.find("delta");
        if (delta == choice.end() || !delta->is_object()) return;
        if (std::string text = string_field(*delta, "content"); !text.empty()) {
            out.emplace_back(TextDelta{std::move(text)});
        }
        if (std::string reasoning = string_field(*delta, "reasoning_content"); !reasoning.empty()) {
            out.emplace_back(ReasoningDelta{std::move(reasoning)});
        }
        if (const auto calls = delta->find("tool_calls"); calls != delta->end() && calls->is_array()) {
            append_tool_calls(*calls, out);
        }
    }

    Error classify(const net::HttpResponse& response) const override {
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
        return error;
    }

private:
    void append_tool_calls(const json& fragments, std::vector<StreamEvent>& out) {
        for (const json& fragment : fragments) {
            if (!fragment.is_object()) continue;
            const int index = fragment.contains("index") && fragment["index"].is_number_integer()
                                  ? fragment["index"].get<int>()
                                  : static_cast<int>(call_order_.size());
            if (started_.insert(index).second) {
                call_order_.push_back(index);
                std::string name;
                if (const auto function = fragment.find("function");
                    function != fragment.end() && function->is_object()) {
                    name = string_field(*function, "name");
                }
                out.emplace_back(ToolCallBegin{index, string_field(fragment, "id"), std::move(name)});
            }
            if (const auto function = fragment.find("function");
                function != fragment.end() && function->is_object()) {
                if (std::string arguments = string_field(*function, "arguments"); !arguments.empty()) {
                    out.emplace_back(ToolCallDelta{index, std::move(arguments)});
                }
            }
        }
    }

    void close(std::vector<StreamEvent>& out) {
        if (closed_) return;
        closed_ = true;
        for (const int index : call_order_) out.emplace_back(ToolCallEnd{index});
        if (usage_) out.emplace_back(*usage_);
        out.emplace_back(Finish{finish_reason_from(finish_reason_), finish_reason_});
    }

    void fail(std::vector<StreamEvent>& out, std::string message) {
        closed_ = true;
        out.emplace_back(Finish{Finish::Reason::error, std::move(message)});
    }

    OpenAiChatOptions opt_;
    bool closed_ = false;
    std::string finish_reason_;
    std::optional<Usage> usage_;
    std::vector<int> call_order_; ///< 各 index 首次出现的顺序，ToolCallEnd 按此顺序补
    std::set<int> started_;
};

} // namespace

std::unique_ptr<Codec> make_openai_chat_codec(OpenAiChatOptions options) {
    return std::make_unique<OpenAiChatCodec>(std::move(options));
}

} // namespace dagent::agent
