#include "llm/model.hpp"
#include "agent/public_model.hpp"
#include "llm/provider_detail.hpp"
#include "base/text.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/log.hpp"
#include "base/overloaded.hpp"
#include "net/sse.hpp"

namespace dagent::llm {
using agent::Finish;
using agent::Message;
using agent::ModelError;
using agent::Reply;
using agent::Request;
using agent::RetryInfo;
using agent::StreamEvent;
using agent::TextDelta;
using agent::ToolCall;
using agent::ToolCallBegin;
using agent::ToolCallDelta;
using agent::ToolCallEnd;
using agent::ToolSpec;
using agent::Usage;
using agent::ReasoningDelta;
using agent::Role;
using namespace std::chrono_literals;
namespace {

std::shared_ptr<spdlog::logger> log_agent() { return base::logger("agent"); }

std::mt19937& rng() {
    static thread_local std::mt19937 engine(std::random_device{}());
    return engine;
}

struct AttemptState {
    Reply reply;
    bool had_output = false;
    bool any_sse = false;
    bool finish_seen = false;
    bool has_usage = false;
    Usage usage;
    std::unordered_map<int, std::size_t> positions; ///< ToolCall index → tool_calls 下标
};

void begin_call(AttemptState& state, const ToolCallBegin& event) {
    if (state.positions.contains(event.index)) return;
    state.positions.emplace(event.index, state.reply.message.tool_calls.size());
    state.reply.message.tool_calls.push_back(ToolCall{event.id, event.name, {}});
}

void append_arguments(AttemptState& state, const ToolCallDelta& event) {
    const auto it = state.positions.find(event.index);
    if (it == state.positions.end()) return;
    state.reply.message.tool_calls[it->second].arguments += event.args_fragment;
}

} // namespace

Model::Model(ProviderConfig provider, net::HttpOptions http,
             agent::RetryOptions retry, Framing framing)
    : provider_(std::move(provider)), http_(http), retry_(retry), framing_(framing) {}

Model::AttemptOutcome Model::attempt(const Request& request, const ProviderConfig& config,
                                      const std::function<void(const StreamEvent&)>& on_event,
                                      std::stop_token stop) {
    AttemptOutcome out;
    std::unique_ptr<Codec> codec = make_codec(config);
    auto encoded = codec->encode(request);
    if (!config.login.empty()) encoded.url = checked_login_url(login_provider(config.login), encoded.url);
    std::vector<std::string> secrets{config.api_key};
    for (const auto& [name, value] : config.headers) secrets.push_back(value);
    // Include substituted header values and JSON-escaped spellings echoed by gateways.
    for (const auto& [name, value] : encoded.headers) {
        if (name == "Content-Type" || name == "Accept" || name == "anthropic-version") continue;
        secrets.push_back(value);
    }
    const auto raw_secrets = secrets;
    for (const auto& secret : raw_secrets) {
        const auto escaped = nlohmann::json(secret).dump();
        secrets.push_back(escaped.substr(1, escaped.size() - 2));
    }
    const auto redact = [&](std::string text) { return provider_detail::redact(std::move(text), secrets); };
    net::SseParser sse;
    AttemptState state;
    state.reply.message.role = Role::assistant;
    std::string pending_line;

    const auto handle = [&](const StreamEvent& event) {
        std::visit(base::Overloaded{
                       [&](const TextDelta& e) {
                           state.reply.message.content += e.text;
                           state.had_output = true;
                       },
                       [&](const ReasoningDelta& e) {
                           state.reply.message.reasoning_content += e.text;
                           state.reply.message.reasoning_signature += e.signature;
                           state.had_output = true;
                       },
                       [&](const ToolCallBegin& e) {
                           begin_call(state, e);
                           state.had_output = true;
                       },
                       [&](const ToolCallDelta& e) { append_arguments(state, e); },
                       [&](const ToolCallEnd&) {},
                       [&](const Usage& e) {
                           state.usage = e;
                           state.has_usage = true;
                       },
                       [&](const Finish& e) {
                           state.finish_seen = true;
                           state.reply.finish = e;
                       },
                   },
                   event);
    };

    const auto decode = [&](const net::SseEvent& event) {
        state.any_sse = true;
        std::vector<StreamEvent> events;
        codec->decode(event, events);
        for (auto& stream_event : events) {
            if (auto* finish = std::get_if<Finish>(&stream_event)) {
                finish->raw = base::to_valid_utf8(redact(finish->raw));
                finish->raw.resize(base::utf8_floor(finish->raw, 300));
            }
            handle(stream_event);
            on_event(stream_event);
        }
    };
    const auto decode_line = [&](std::string_view line) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.find_first_not_of(" \t") != std::string_view::npos)
            decode(net::SseEvent{{}, std::string(line), {}});
    };
    const auto on_data = [&](std::string_view chunk) {
        if (framing_ == Framing::sse) { sse.feed(chunk, decode); return; }
        pending_line.append(chunk);
        std::size_t begin = 0, end;
        while ((end = pending_line.find('\n', begin)) != std::string::npos) {
            decode_line(std::string_view(pending_line).substr(begin, end - begin));
            begin = end + 1;
        }
        pending_line.erase(0, begin);
    };

    const auto finish_outcome = [&](AttemptOutcome::Kind kind, std::string message) {
        out.kind = kind;
        out.message = redact(std::move(message));
        out.had_output = state.had_output;
        out.reply = std::move(state.reply);
        if (state.has_usage) out.reply.usage = state.usage;
        return out;
    };

    net::HttpResponse response;
    try {
        response = http_.stream(encoded, on_data, stop);
    } catch (const net::HttpError& e) {
        if (e.kind() == net::HttpError::Kind::cancelled || stop.stop_requested()) {
            return finish_outcome(AttemptOutcome::Kind::cancelled, "interrupted");
        }
        switch (e.kind()) {
        case net::HttpError::Kind::connect:
        case net::HttpError::Kind::transport:
        case net::HttpError::Kind::timeout:
            return finish_outcome(AttemptOutcome::Kind::retryable, e.what());
        case net::HttpError::Kind::cancelled: break;
        case net::HttpError::Kind::tls:
        case net::HttpError::Kind::too_large:
            return finish_outcome(AttemptOutcome::Kind::rejected, e.what());
        }
        return finish_outcome(AttemptOutcome::Kind::rejected, e.what());
    }

    if (stop.stop_requested()) return finish_outcome(AttemptOutcome::Kind::cancelled, "interrupted");

    if (response.status == 401 && !config.login.empty()) {
        state.has_usage = true;
        state.usage = Usage{}; // A rejected authentication request did not run inference.
        return finish_outcome(AttemptOutcome::Kind::unauthorized, "OpenCode needs sign-in; use dagent login opencode or /model");
    }
    if (response.status < 200 || response.status >= 300) {
        if (response.body_truncated) {
            // A transport cutoff can split a secret; never expose the retained prefix.
            response.body = "provider error body exceeded the configured limit";
        } else {
            auto body = nlohmann::json::parse(response.body, nullptr, false);
            if (!body.is_discarded()) {
                const auto clean = [&](auto&& self, nlohmann::json& value) -> void {
                    if (value.is_string()) value = redact(value.get<std::string>());
                    else if (value.is_structured()) for (auto& child : value) self(self, child);
                };
                clean(clean, body);
                response.body = body.dump();
            }
            response.body = redact(response.body);
        }
        const Error error = codec->classify(response);
        out.retry_after = error.retry_after;
        if (error.context_too_long) return finish_outcome(AttemptOutcome::Kind::context_too_long, error.message);
        if (error.retryable) return finish_outcome(AttemptOutcome::Kind::retryable, error.message);
        return finish_outcome(AttemptOutcome::Kind::rejected, error.message);
    }
    if (framing_ == Framing::ndjson && !pending_line.empty()) decode_line(pending_line);
    if (!state.any_sse) {
        return finish_outcome(AttemptOutcome::Kind::rejected, "The provider returned no event stream - it may not support stream=true");
    }
    if (state.finish_seen && state.reply.finish.reason == Finish::Reason::error) {
        const std::string detail = state.reply.finish.raw.empty() ? "provider returned an error" : state.reply.finish.raw;
        return finish_outcome(AttemptOutcome::Kind::retryable, "stream did not finish normally: " + detail);
    }
    if (!state.finish_seen) {
        return finish_outcome(AttemptOutcome::Kind::retryable, "stream ended without finish_reason before the connection closed");
    }

    out.kind = AttemptOutcome::Kind::success;
    out.had_output = state.had_output;
    out.reply = std::move(state.reply);
    if (state.has_usage) out.reply.usage = state.usage;

    // 个别网关不给 id，tool 消息必须能对上；同一条回复里重复的 id 会让下一次请求 400。
    std::map<std::string, int> seen;
    int sequence = 0;
    for (ToolCall& call : out.reply.message.tool_calls) {
        ++sequence;
        if (call.id.empty()) call.id = std::format("call_{}_{}", ++call_serial_, sequence);
        const auto [it, inserted] = seen.emplace(call.id, 1);
        if (!inserted) call.id += std::format("_{}", it->second++);
    }
    return out;
}

agent::Reply Model::complete(const agent::Request& request,
                             const std::function<void(const agent::StreamEvent&)>& on_event,
                      const std::function<void(const RetryInfo&)>& on_retry, std::stop_token stop,
                      const agent::ModelAttemptHooks& attempts) {
    bool recovered = false;
    std::string rejected_token;
    std::optional<Request> resend;
    for (int retries = 0;;) {
        if (stop.stop_requested()) throw ModelError(ModelError::Kind::cancelled, {}, "interrupted");
        ProviderConfig config = provider_;
        if (!config.login.empty()) {
            try {
                config.api_key = request_credentials(login_provider(config.login), config.credentials, stop, rejected_token).access_token;
            } catch (const std::exception& error) {
                throw ModelError(stop.stop_requested() ? ModelError::Kind::cancelled : ModelError::Kind::rejected, {}, error.what());
            }
        }
        Request current = resend.value_or(request);
        const auto previous_limit = current.max_tokens;
        if (attempts.before) attempts.before(current);
        if (resend && current.max_tokens != previous_limit) {
            Reply unused;
            unused.usage = Usage{};
            if (attempts.after) attempts.after(unused);
            throw ModelError(ModelError::Kind::budget_exhausted, {}, "insufficient budget to resend the authenticated request unchanged");
        }
        resend.reset();
        rejected_token.clear();
        const auto started = std::chrono::steady_clock::now();
        AttemptOutcome out;
        try { out = attempt(current, config, on_event, stop); }
        catch (const std::invalid_argument& error) {
            throw ModelError(ModelError::Kind::rejected, {}, error.what());
        } catch (const std::exception&) {
            throw ModelError(stop.stop_requested() ? ModelError::Kind::cancelled : ModelError::Kind::rejected,
                             {}, "model request encoding or stream decoding failed");
        }
        if (attempts.after) attempts.after(out.reply);
        switch (out.kind) {
        case AttemptOutcome::Kind::success: {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            const Usage usage = out.reply.usage.value_or(Usage{});
            log_agent()->info("模型调用完成：{}ms finish={} prompt={} completion={} cached={}",
                              elapsed.count(), out.reply.finish.raw, usage.prompt, usage.completion,
                              usage.cached);
            return std::move(out.reply);
        }
        case AttemptOutcome::Kind::cancelled:
            throw ModelError(ModelError::Kind::cancelled, std::move(out.reply), out.message);
        case AttemptOutcome::Kind::context_too_long:
            throw ModelError(ModelError::Kind::context_too_long, std::move(out.reply), out.message);
        case AttemptOutcome::Kind::rejected:
            throw ModelError(ModelError::Kind::rejected, std::move(out.reply), out.message);
        case AttemptOutcome::Kind::unauthorized:
            if (recovered) throw ModelError(ModelError::Kind::rejected, std::move(out.reply), out.message);
            recovered = true;
            rejected_token = config.api_key;
            resend = current;
            continue;
        case AttemptOutcome::Kind::retryable: break;
        }

        if (retries >= retry_.max_retries) {
            throw ModelError(ModelError::Kind::exhausted, std::move(out.reply), out.message);
        }
        if (out.retry_after > retry_.max_retry_after) {
            throw ModelError(ModelError::Kind::exhausted, std::move(out.reply),
                             std::format("provider requested a retry in {} seconds, which is too long: {}",
                                         std::chrono::duration_cast<std::chrono::seconds>(out.retry_after).count(),
                                         out.message));
        }

        auto wait = std::chrono::milliseconds{retry_.base_delay};
        for (int i = 0; i < retries; ++i) wait = std::min(retry_.max_delay, wait * 2);
        std::uniform_real_distribution<double> jitter(0.8, 1.2);
        wait = std::chrono::milliseconds{
            static_cast<std::int64_t>(static_cast<double>(wait.count()) * jitter(rng()))};
        if (out.retry_after > wait) wait = out.retry_after;

        const int attempt_number = ++retries;
        log_agent()->warn("模型调用失败（第 {}/{} 次重试，等待 {}ms{}）：{}", attempt_number,
                          retry_.max_retries, wait.count(), out.had_output ? "，丢弃已显示的内容" : "",
                          out.message);
        on_retry(RetryInfo{attempt_number, retry_.max_retries, wait, out.message, out.had_output});

        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait_for(lock, stop, wait, [] { return false; });
        if (stop.stop_requested()) throw ModelError(ModelError::Kind::cancelled, Reply{}, "interrupted");
    }
}

} // namespace dagent::llm

namespace dagent::llm {
std::shared_ptr<agent::ModelSession> make_session(const ProviderConfig& provider, net::HttpOptions http,
                                                  agent::RetryOptions retry) {
    if (!provider.login.empty()) { http.verify_peer = true; http.verify_host = true; }
    return std::make_shared<Model>(provider, std::move(http),
                                   retry, find_provider(provider.kind)->framing);
}

agent::PublicModel to_public(const ProviderConfig& provider) {
    agent::PublicModel out;
    out.name = provider.name;
    out.kind = provider.kind;
    out.model = provider.model;
    out.base_url = provider.base_url;
    out.max_tokens = provider.max_tokens;
    out.temperature = provider.temperature;
    out.context_window = provider.context_window;
    out.send_reasoning_content = provider.send_reasoning_content;
    out.has_key = !provider.api_key.empty();
    out.auth = !provider.login.empty() ? "login:" + provider.login : out.has_key ? "key" : "";
    return out;
}
} // namespace dagent::llm
