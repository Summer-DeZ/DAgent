#include "agent/provider_detail.hpp"
#include "session/session.hpp"

#include <map>
#include <utility>

namespace dagent::agent::provider_detail {
namespace {
using nlohmann::json;
class OllamaCodec final : public Codec {
public:
    explicit OllamaCodec(ProviderConfig config) : config_(std::move(config)) {}

    net::HttpRequest encode(const Request& request) const override {
        json messages = json::array();
        std::map<std::string, std::string> tool_names;
        for (const auto& message : request.messages) {
            json encoded = {{"role", role_name(message.role)}, {"content", message.content}};
            if (config_.send_reasoning_content && !message.reasoning_content.empty())
                encoded["thinking"] = message.reasoning_content;
            if (!message.tool_calls.empty()) {
                encoded["tool_calls"] = json::array();
                for (const auto& call : message.tool_calls) {
                    tool_names[call.id] = call.name;
                    encoded["tool_calls"].push_back({{"function", {{"name", call.name},
                                                      {"arguments", json::parse(call.arguments)}}}});
                }
            }
            if (message.role == Role::tool) encoded["tool_name"] = tool_names.at(message.tool_call_id);
            messages.push_back(std::move(encoded));
        }
        json options = {{"num_ctx", config_.context_window}};
        if (request.max_tokens > 0) options["num_predict"] = request.max_tokens;
        if (request.temperature >= 0) options["temperature"] = request.temperature;
        // options 是唯一需要合并的嵌套对象；保留显式预算，允许其余采样参数。
        if (const auto extra = config_.extra_body.find("options"); extra != config_.extra_body.end())
            merge_extra(options, *extra);
        json body = {{"model", request.model}, {"messages", std::move(messages)},
                     {"stream", request.stream}, {"options", std::move(options)}};
        if (!request.tools.empty()) body["tools"] = encode_tools(request.tools);
        merge_extra(body, config_.extra_body);
        net::HttpRequest http;
        http.method = "POST";
        http.url = join_url(config_.base_url, "/api/chat");
        http.headers = {{"Content-Type", "application/json"}};
        http.body = body.dump();
        return http;
    }
    void decode(const net::SseEvent& event, std::vector<StreamEvent>& out) override {
        if (closed_) return;
        const auto chunk = json::parse(event.data, nullptr, false);
        if (!chunk.is_object()) { fail(out, "invalid JSON in NDJSON stream"); return; }
        if (chunk.contains("error")) { fail(out, error_message(chunk["error"])); return; }
        if (const auto message = chunk.find("message"); message != chunk.end() && message->is_object()) {
            if (auto text = string_field(*message, "content"); !text.empty()) out.emplace_back(TextDelta{std::move(text)});
            if (auto text = string_field(*message, "thinking"); !text.empty()) out.emplace_back(ReasoningDelta{std::move(text)});
            if (const auto calls = message->find("tool_calls"); calls != message->end() && calls->is_array()) {
                for (const auto& call : *calls) {
                    const auto function = call.value("function", json::object());
                    const int index = next_call_++;
                    out.emplace_back(ToolCallBegin{index, call_prefix_ + std::to_string(index), string_field(function, "name")});
                    out.emplace_back(ToolCallDelta{index, function.value("arguments", json::object()).dump()});
                    out.emplace_back(ToolCallEnd{index});
                }
            }
        }
        if (chunk.value("done", false)) {
            closed_ = true;
            out.emplace_back(Usage{int_field(chunk, "prompt_eval_count"), int_field(chunk, "eval_count"),
                                   int_field(chunk, "prompt_eval_cached_count")});
            const auto raw = string_field(chunk, "done_reason");
            Finish::Reason reason = Finish::Reason::error;
            if (raw == "stop") reason = next_call_ > 0 ? Finish::Reason::tool_calls : Finish::Reason::stop;
            else if (raw == "length") reason = Finish::Reason::length;
            out.emplace_back(Finish{reason, raw});
        }
    }
    Error classify(const net::HttpResponse& response) const override { return classify_http(response, config_.api_key); }
private:
    void fail(std::vector<StreamEvent>& out, std::string message) {
        closed_ = true;
        out.emplace_back(Finish{Finish::Reason::error, std::move(message)});
    }
    ProviderConfig config_;
    const std::string call_prefix_ = "call_" + session::new_id() + "_";
    int next_call_ = 0;
    bool closed_ = false;
};
} // namespace
std::unique_ptr<Codec> make_ollama(const ProviderConfig& config) { return std::make_unique<OllamaCodec>(config); }
} // namespace dagent::agent::provider_detail
