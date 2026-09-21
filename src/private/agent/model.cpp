#include "agent/model.hpp"

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
#include "net/sse.hpp"

namespace dagent::agent {
namespace {

std::shared_ptr<spdlog::logger> log_agent() { return base::logger("agent"); }

std::mt19937& rng() {
    static thread_local std::mt19937 engine(std::random_device{}());
    return engine;
}

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

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

ModelError::ModelError(Kind kind, Reply partial, const std::string& what)
    : std::runtime_error(what), kind_(kind), partial_(std::move(partial)) {}

Model::Model(std::function<std::unique_ptr<Codec>()> codec_factory, net::HttpOptions http, RetryOptions retry, Framing framing)
    : codec_factory_(std::move(codec_factory)), http_(http), retry_(retry), framing_(framing) {}

Model::AttemptOutcome Model::attempt(const Request& request,
                                      const std::function<void(const StreamEvent&)>& on_event,
                                      std::stop_token stop) {
    AttemptOutcome out;
    std::unique_ptr<Codec> codec = codec_factory_();
    net::SseParser sse;
    AttemptState state;
    state.reply.message.role = Role::assistant;
    std::string pending_line;

    const auto handle = [&](const StreamEvent& event) {
        std::visit(Overloaded{
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
        for (const auto& stream_event : events) { handle(stream_event); on_event(stream_event); }
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
        out.message = std::move(message);
        out.had_output = state.had_output;
        out.reply = std::move(state.reply);
        return out;
    };

    net::HttpResponse response;
    try {
        response = http_.stream(codec->encode(request), on_data, stop);
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

    if (response.status < 200 || response.status >= 300) {
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

Reply Model::complete(const Request& request, const std::function<void(const StreamEvent&)>& on_event,
                      const std::function<void(const RetryInfo&)>& on_retry, std::stop_token stop) {
    for (int retries = 0;; ++retries) {
        const auto started = std::chrono::steady_clock::now();
        AttemptOutcome out = attempt(request, on_event, stop);
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

        const int attempt_number = retries + 1;
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

} // namespace dagent::agent
