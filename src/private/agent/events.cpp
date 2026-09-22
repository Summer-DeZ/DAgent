#include "agent/events.hpp"

#include <string>

#include "base/text.hpp"

namespace dagent::agent {
namespace {

using nlohmann::json;

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

json usage_json(const Usage& usage) {
    return json{{"prompt", usage.prompt}, {"completion", usage.completion}, {"cached", usage.cached}};
}

const char* sandbox_name(SandboxProfile mode) {
    switch (mode) {
    case SandboxProfile::read_only: return "read_only";
    case SandboxProfile::workspace_write: return "workspace_write";
    case SandboxProfile::full_access: return "full_access";
    }
    return "unknown";
}

const char* grant_source_name(GrantSource source) {
    switch (source) {
    case GrantSource::mode: return "mode";
    case GrantSource::once: return "once";
    case GrantSource::session: return "session";
    case GrantSource::unrestricted: return "unrestricted";
    }
    return "mode";
}

json paths_json(const std::vector<std::filesystem::path>& paths) {
    json result = json::array();
    for (const auto& path : paths) result.push_back(path.string());
    return result;
}

const char* level_name(Notice::Level level) {
    switch (level) {
    case Notice::Level::info: return "info";
    case Notice::Level::warn: return "warn";
    case Notice::Level::error: return "error";
    }
    return "info";
}

} // namespace

std::string_view to_string(TurnStatus status) {
    switch (status) {
    case TurnStatus::done: return "done";
    case TurnStatus::interrupted: return "interrupted";
    case TurnStatus::denied: return "denied";
    case TurnStatus::limit: return "limit";
    case TurnStatus::failed: return "failed";
    }
    return "unknown";
}

// 字段名与文法见 docs/design/agent.md §2。
json to_json(const Event& event) {
    return std::visit(
        Overloaded{
            [](const TurnStarted& e) { return json{{"type", "turn_started"}, {"input", e.input}}; },
            [](const StepStarted& e) { return json{{"type", "step_started"}, {"step", e.step}}; },
            [](const TextDelta& e) { return json{{"type", "text"}, {"text", e.text}}; },
            [](const ReasoningDelta& e) { return json{{"type", "reasoning"}, {"text", e.text}}; },
            [](const StreamReset&) { return json{{"type", "stream_reset"}}; },
            [](const ToolPending& e) {
                return json{{"type", "tool_pending"}, {"id", e.id}, {"name", e.name}};
            },
            [](const ToolStarted& e) {
                return json{{"type", "tool_started"},
                            {"id", e.id},
                            {"name", e.name},
                            {"summary", e.summary},
                            {"sandbox", sandbox_name(e.grant.sandbox)},
                            {"backend", e.grant.backend},
                            {"grant_source", grant_source_name(e.grant.source)},
                            {"analysis_version", e.grant.analysis_version},
                            {"network", e.grant.allow_network},
                            {"local_sockets", e.grant.allow_local_sockets},
                            {"private_tmp", e.grant.private_tmp},
                            {"protect_sensitive_names", e.grant.protect_sensitive_names},
                            {"readable", paths_json(e.grant.readable)},
                            {"writable", paths_json(e.grant.writable)},
                            {"protected_read", paths_json(e.grant.protected_read)},
                            {"protected_write", paths_json(e.grant.protected_write)},
                            {"network_targets", e.grant.network_targets}};
            },
            [](const ToolOutput& e) {
                // chunk 是原始字节，先过一遍 UTF-8 再进 JSON（docs/design/agent.md §2）。
                return json{{"type", "tool_output"},
                            {"id", e.id},
                            {"chunk", base::to_valid_utf8(e.chunk)}};
            },
            [](const ToolFinished& e) {
                return json{{"type", "tool_finished"},
                            {"id", e.id},
                            {"name", e.name},
                            {"summary", e.summary},
                            {"text", e.result.model_text},
                            {"is_error", e.result.is_error},
                            {"interrupted", e.result.interrupted},
                            {"view", to_json(e.result.display)}};
            },
            [](const SubEvent& e) {
                return json{{"type", "sub_event"},
                            {"session", e.session},
                            {"agent", e.agent},
                            {"parent_call", e.call_id},
                            {"event", to_json(e.event())}};
            },
            [](const Retrying& e) {
                return json{{"type", "retrying"},
                            {"attempt", e.attempt},
                            {"max_attempts", e.max_attempts},
                            {"wait_ms", e.wait.count()},
                            {"reason", e.reason}};
            },
            [](const Compacted& e) {
                return json{{"type", "compacted"},
                            {"before", e.before},
                            {"after", e.after},
                            {"summarized", e.summarized}};
            },
            [](const ContextUpdate& e) {
                json j = {{"type", "context"}, {"used", e.used}, {"limit", e.limit}};
                j.update(usage_json(e.usage));
                return j;
            },
            [](const ModelChanged& e) { return json{{"type", "model_changed"}, {"model", e.model}}; },
            [](const ModeChanged& e) {
                return json{{"type", "mode_changed"}, {"mode", e.mode}, {"planning", e.planning}};
            },
            [](const Notice& e) {
                return json{{"type", "notice"}, {"level", level_name(e.level)}, {"text", e.text}};
            },
            [](const TurnEnded& e) {
                return json{{"type", "turn_ended"},
                            {"status", std::string(to_string(e.status))},
                            {"error", e.error},
                            {"steps", e.steps},
                            {"tool_calls", e.tool_calls},
                            {"usage", usage_json(e.total)}};
            },
        },
        event);
}

} // namespace dagent::agent
