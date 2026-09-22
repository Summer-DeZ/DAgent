#include "llm/provider_detail.hpp"

#include <map>
#include <utility>

namespace dagent::llm::provider_detail {
using agent::Finish;
using agent::Message;
using agent::ReasoningDelta;
using agent::Role;
using agent::StreamEvent;
using agent::TextDelta;
using agent::ToolCall;
using agent::ToolCallBegin;
using agent::ToolCallDelta;
using agent::ToolCallEnd;
using agent::ToolSpec;
using agent::Usage;
namespace {
using nlohmann::json;

class AnthropicCodec final : public Codec {
public:
    explicit AnthropicCodec(ProviderConfig config) : config_(std::move(config)) {}

    net::HttpRequest encode(const agent::Request& request) const override {
        json messages = json::array();
        std::string system;
        for (const auto& message : request.messages) {
            if (message.role == Role::system) {
                if (!system.empty()) system += "\n\n";
                system += message.content;
                continue;
            }
            const std::string role = message.role == Role::assistant ? "assistant" : "user";
            json content = json::array();
            if (message.role == Role::tool) {
                content.push_back({{"type", "tool_result"}, {"tool_use_id", message.tool_call_id},
                                   {"content", message.content}});
            } else {
                if (!message.content.empty()) content.push_back({{"type", "text"}, {"text", message.content}});
                for (const auto& call : message.tool_calls) {
                    content.push_back({{"type", "tool_use"}, {"id", call.id}, {"name", call.name},
                                       {"input", json::parse(call.arguments)}});
                }
            }
            if (content.empty()) continue;
            // 并行工具结果必须留在同一条 user 消息，紧跟 assistant 的 tool_use。
            if (!messages.empty() && messages.back()["role"] == role) {
                for (auto& block : content) messages.back()["content"].push_back(std::move(block));
            } else messages.push_back({{"role", role}, {"content", std::move(content)}});
        }
        json body = {{"model", request.model}, {"messages", std::move(messages)},
                     {"max_tokens", request.max_tokens}, {"stream", request.stream}};
        if (!system.empty()) body["system"] = system;
        if (request.temperature >= 0) body["temperature"] = request.temperature;
        if (!request.tools.empty()) {
            body["tools"] = json::array();
            for (const auto& tool : request.tools)
                body["tools"].push_back({{"name", tool.name}, {"description", tool.description},
                                         {"input_schema", tool.parameters}});
        }
        merge_extra(body, config_.extra_body);
        net::HttpRequest http;
        http.method = "POST";
        http.url = join_url(config_.base_url, "/messages");
        http.headers = {{"Content-Type", "application/json"}, {"x-api-key", config_.api_key},
                        {"anthropic-version", "2023-06-01"}};
        http.body = body.dump();
        return http;
    }

    void decode(const net::SseEvent& event, std::vector<StreamEvent>& out) override {
        if (closed_) return;
        const auto chunk = json::parse(event.data, nullptr, false);
        if (!chunk.is_object()) { fail(out, "invalid JSON in message stream"); return; }
        const std::string type = string_field(chunk, "type");
        if (type == "error") { fail(out, error_message(chunk.value("error", json::object()))); return; }
        if (type == "message_start") {
            const auto message = chunk.value("message", json::object());
            const auto usage = message.value("usage", json::object());
            usage_.cached = int_field(usage, "cache_read_input_tokens");
            usage_.prompt = int_field(usage, "input_tokens") + usage_.cached + int_field(usage, "cache_creation_input_tokens");
            usage_.completion = int_field(usage, "output_tokens");
        } else if (type == "content_block_start") {
            const int index = int_field(chunk, "index");
            const auto block = chunk.value("content_block", json::object());
            const auto kind = string_field(block, "type");
            if (kind == "tool_use") {
                calls_.emplace(index, false);
                out.emplace_back(ToolCallBegin{index, string_field(block, "id"), string_field(block, "name")});
                if (const auto input = block.find("input"); input != block.end() && !input->empty()) {
                    calls_[index] = true;
                    out.emplace_back(ToolCallDelta{index, input->dump()});
                }
            } else if (kind == "text") {
                if (auto text = string_field(block, "text"); !text.empty()) out.emplace_back(TextDelta{std::move(text)});
            }
        } else if (type == "content_block_delta") {
            const auto delta = chunk.value("delta", json::object());
            const auto kind = string_field(delta, "type");
            const int index = int_field(chunk, "index");
            if (kind == "text_delta") out.emplace_back(TextDelta{string_field(delta, "text")});
            else if (kind == "thinking_delta") out.emplace_back(ReasoningDelta{string_field(delta, "thinking")});
            else if (kind == "signature_delta") out.emplace_back(ReasoningDelta{{}, string_field(delta, "signature")});
            else if (kind == "input_json_delta") {
                if (!calls_.contains(index)) { fail(out, "tool delta without tool start"); return; }
                const auto fragment = string_field(delta, "partial_json");
                if (!fragment.empty()) calls_[index] = true;
                out.emplace_back(ToolCallDelta{index, fragment});
            }
        } else if (type == "content_block_stop") {
            const int index = int_field(chunk, "index");
            if (const auto call = calls_.find(index); call != calls_.end()) {
                if (!call->second) out.emplace_back(ToolCallDelta{index, "{}"});
                out.emplace_back(ToolCallEnd{index});
                calls_.erase(call);
            }
        } else if (type == "message_delta") {
            const auto delta = chunk.value("delta", json::object());
            const auto raw = string_field(delta, "stop_reason");
            // output_tokens 是累计值，不能与 message_start 重复相加。
            const auto usage = chunk.value("usage", json::object());
            if (usage.contains("output_tokens")) usage_.completion = int_field(usage, "output_tokens");
            out.emplace_back(usage_);
            Finish::Reason reason = Finish::Reason::error;
            if (raw == "end_turn" || raw == "stop_sequence") reason = Finish::Reason::stop;
            else if (raw == "max_tokens") reason = Finish::Reason::length;
            else if (raw == "tool_use") reason = Finish::Reason::tool_calls;
            else if (raw == "refusal") reason = Finish::Reason::content_filter;
            out.emplace_back(Finish{reason, raw});
        } else if (type == "message_stop") closed_ = true;
    }

    Error classify(const net::HttpResponse& response) const override { return classify_http(response, config_.api_key); }

private:
    void fail(std::vector<StreamEvent>& out, std::string message) {
        closed_ = true;
        out.emplace_back(Finish{Finish::Reason::error, std::move(message)});
    }
    ProviderConfig config_;
    Usage usage_;
    std::map<int, bool> calls_;
    bool closed_ = false;
};
} // namespace
std::unique_ptr<Codec> make_anthropic(const ProviderConfig& config) { return std::make_unique<AnthropicCodec>(config); }
} // namespace dagent::llm::provider_detail
