#include "llm/provider_detail.hpp"

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

namespace dagent::llm {
using agent::Finish;
using agent::Message;
using agent::Role;
using agent::StreamEvent;
using agent::TextDelta;
using agent::ToolCall;
using agent::ToolCallBegin;
using agent::ToolCallDelta;
using agent::ToolCallEnd;
using agent::ToolSpec;
using agent::ReasoningDelta;
using agent::Request;
using agent::Usage;
namespace {

using nlohmann::json;
using namespace provider_detail;

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

class OpenAiChatCodec final : public Codec {
public:
    explicit OpenAiChatCodec(ProviderConfig options) : opt_(std::move(options)) {}

    net::HttpRequest encode(const agent::Request& request) const override {
        json body = json::object();
        body["model"] = request.model;
        body["messages"] = encode_messages(request.messages, opt_.send_reasoning_content);
        body["stream"] = request.stream;
        if (request.stream && opt_.include_usage) {
            body["stream_options"] = {{"include_usage", true}};
        }
        if (request.max_tokens > 0 && !opt_.extra_body.contains("max_completion_tokens"))
            body["max_tokens"] = request.max_tokens;
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
            fail(out, std::format("invalid JSON in SSE data: {}", std::string(data.substr(0, 120))));
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
        if (std::string reasoning = string_field(*delta, delta->contains("reasoning_content") ? "reasoning_content" : "reasoning"); !reasoning.empty()) {
            out.emplace_back(ReasoningDelta{std::move(reasoning)});
        }
        if (const auto calls = delta->find("tool_calls"); calls != delta->end() && calls->is_array()) {
            append_tool_calls(*calls, out);
        }
    }

    Error classify(const net::HttpResponse& response) const override {
        return classify_http(response, opt_.api_key);
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

    ProviderConfig opt_;
    bool closed_ = false;
    std::string finish_reason_;
    std::optional<Usage> usage_;
    std::vector<int> call_order_; ///< 各 index 首次出现的顺序，ToolCallEnd 按此顺序补
    std::set<int> started_;
};

} // namespace

std::unique_ptr<Codec> provider_detail::make_chat(const ProviderConfig& options) {
    return std::make_unique<OpenAiChatCodec>(options);
}

} // namespace dagent::llm
