#include "agent/record.hpp"

#include <format>
#include <utility>

#include "base/log.hpp"

namespace dagent::agent {
namespace {

using nlohmann::json;

std::shared_ptr<spdlog::logger> log_agent() { return base::logger("agent"); }

const char* answer_name(Decision::Answer answer) {
    switch (answer) {
    case Decision::Answer::allow: return "allow";
    case Decision::Answer::allow_session: return "allow_session";
    case Decision::Answer::deny: return "deny";
    case Decision::Answer::deny_with_feedback: return "deny_with_feedback";
    }
    return "deny";
}

json usage_json(const Usage& usage) {
    return json{{"prompt", usage.prompt}, {"completion", usage.completion}, {"cached", usage.cached}};
}

} // namespace

Recorder Recorder::create(const session::Options& options, session::Meta meta) {
    return Recorder(session::Writer::create(options, std::move(meta)));
}

Recorder Recorder::resume(const session::Options& options, std::string_view id) {
    return Recorder(session::Writer::resume(options, id));
}

void Recorder::append(std::string_view type, nlohmann::json payload) {
    if (broken_ || !writer_) return;
    try {
        writer_->append(type, std::move(payload));
    } catch (const session::SessionError& e) {
        broken_ = true;
        error_ = e.what();
        log_agent()->error("会话记录写入失败：{}", e.what());
    }
}

void Recorder::system(std::string_view text) {
    append("system", json{{"schema", 1}, {"text", text}});
}

void Recorder::user(std::int64_t n, std::string_view text) {
    append("user", json{{"n", n}, {"text", text}});
}

void Recorder::assistant(std::int64_t n, const Reply& reply) {
    json tool_calls = json::array();
    for (const ToolCall& call : reply.message.tool_calls) {
        tool_calls.push_back({{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}});
    }
    json payload = {{"n", n},
                    {"content", reply.message.content},
                    {"reasoning", reply.message.reasoning_content},
                    {"tool_calls", std::move(tool_calls)},
                    {"finish", reply.finish.raw}};
    if (reply.usage) payload["usage"] = usage_json(*reply.usage);
    append("assistant", std::move(payload));
}

void Recorder::tool(std::int64_t n, const ToolCall& call, std::string_view summary,
                    const tools::Result& result) {
    append("tool", json{{"n", n},
                        {"call_id", call.id},
                        {"name", call.name},
                        {"summary", summary},
                        {"text", result.text},
                        {"is_error", result.is_error},
                        {"interrupted", result.interrupted},
                        {"view", tools::to_json(result.display)}});
}

void Recorder::permission(std::string_view call_id, const Decision& decision, std::string_view rule) {
    append("permission", json{{"call_id", call_id},
                              {"answer", answer_name(decision.answer)},
                              {"rule", rule},
                              {"network", decision.network}});
}

void Recorder::turn_end(TurnStatus status, std::string_view error, int steps, int tool_calls,
                        const Usage& total) {
    append("turn_end", json{{"status", to_string(status)},
                            {"error", error},
                            {"steps", steps},
                            {"tool_calls", tool_calls},
                            {"usage", usage_json(total)}});
}

void Recorder::turn_end_crashed() {
    append("turn_end", json{{"status", "crashed"},
                            {"error", ""},
                            {"steps", 0},
                            {"tool_calls", 0},
                            {"usage", usage_json(Usage{})}});
}

void Recorder::sync() {
    if (broken_ || !writer_) return;
    try {
        writer_->sync();
    } catch (const session::SessionError& e) {
        broken_ = true;
        error_ = e.what();
        log_agent()->error("会话记录刷盘失败：{}", e.what());
    }
}

const session::Meta& Recorder::meta() const { return writer_->meta(); }

} // namespace dagent::agent
