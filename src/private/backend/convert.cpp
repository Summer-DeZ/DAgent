#include "backend/convert.hpp"

#include <variant>

namespace dagent::backend {
namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

const char* mcp_status(agent::McpServerState::Status status) {
    switch (status) {
    case agent::McpServerState::Status::connecting: return "connecting";
    case agent::McpServerState::Status::ready: return "ready";
    case agent::McpServerState::Status::failed: return "failed";
    case agent::McpServerState::Status::disconnected: return "disconnected";
    case agent::McpServerState::Status::reconnecting: return "reconnecting";
    }
    return "connecting";
}

const char* mcp_boundary(std::string_view boundary) {
    if (boundary == "remote") return "remote";
    if (boundary == "srt") return "srt";
    return "unknown";
}

std::string preview(std::string_view text) {
    const std::size_t newline = text.find('\n');
    std::string line(text.substr(0, newline == std::string_view::npos ? text.size() : newline));
    if (line.size() > 80) {
        std::size_t cut = 80;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
        line.resize(cut);
    }
    return line;
}

} // namespace

protocol::PublicModel to_protocol(const agent::PublicModel& model) {
    protocol::PublicModel out;
    out.name = model.name;
    out.kind = model.kind;
    out.model = model.model;
    out.base_url = model.base_url;
    out.max_tokens = model.max_tokens;
    out.temperature = model.temperature;
    out.context_window = model.context_window;
    out.has_key = model.has_key;
    return out;
}

protocol::SessionSnapshot to_protocol(const runtime::RuntimeSnapshot& snapshot) {
    protocol::SessionSnapshot out;
    out.session_id = snapshot.session_id;
    out.session_generation = snapshot.generation;
    out.model = to_protocol(snapshot.model);
    out.permission_mode = std::string(agent::to_string(snapshot.permission_mode));
    out.planning = snapshot.planning;
    out.read_only = snapshot.read_only;
    out.busy = snapshot.busy;
    if (!snapshot.operation.empty() || !snapshot.run_id.empty()) {
        protocol::OperationInfo operation;
        operation.kind = snapshot.operation;
        operation.run_id = snapshot.run_id;
        out.current_operation = std::move(operation);
    }
    for (const runtime::QueuedInput& input : snapshot.queue) {
        out.queue.push_back({input.id, preview(input.text)});
    }
    out.context.used = snapshot.used_tokens;
    out.context.limit = snapshot.token_limit;
    out.context.window = snapshot.window_tokens;
    out.context.trigger_percent = snapshot.trigger_percent;
    out.work_plan = agent::to_json(agent::View(snapshot.work_plan));
    for (const agent::McpServerState& state : snapshot.mcp) {
        out.mcp.push_back(nlohmann::json{{"name", state.name},
                                         {"status", mcp_status(state.status)},
                                         {"tools", state.tools},
                                         {"error", state.error},
                                         {"boundary", mcp_boundary(state.boundary)}});
    }
    out.recording.broken = snapshot.recording_broken;
    out.recording.error = snapshot.recording_error;
    return out;
}

protocol::HistoryItem to_protocol(const agent::HistoryItem& item) {
    protocol::HistoryItem out;
    switch (item.kind) {
    case agent::HistoryItem::Kind::user: out.kind = "user"; break;
    case agent::HistoryItem::Kind::assistant: out.kind = "assistant"; break;
    case agent::HistoryItem::Kind::tool: out.kind = "tool"; break;
    case agent::HistoryItem::Kind::tool_started: out.kind = "tool_started"; break;
    case agent::HistoryItem::Kind::system: out.kind = "system"; break;
    case agent::HistoryItem::Kind::turn_end: out.kind = "turn_end"; break;
    }
    out.seq = item.seq;
    out.text = item.text;
    out.reasoning = item.reasoning;
    out.finish = item.finish;
    out.model = item.model;
    out.call_id = item.call_id;
    out.name = item.name;
    out.summary = item.summary;
    if (item.kind == agent::HistoryItem::Kind::tool) {
        out.result = nlohmann::json::object();
        out.result["text"] = item.result.model_text;
        out.result["is_error"] = item.result.is_error;
        out.result["interrupted"] = item.result.interrupted;
        out.result["view"] = agent::to_json(item.result.display);
    }
    if (item.kind == agent::HistoryItem::Kind::tool_started) {
        nlohmann::json json = agent::to_json(agent::Event{item.started});
        json.erase("type");
        out.started = std::move(json);
    }
    out.status = std::string(agent::to_string(item.status));
    out.error = item.error;
    out.steps = item.steps;
    out.tool_calls = item.tool_calls;
    out.usage = nlohmann::json{{"prompt", item.usage.prompt},
                               {"completion", item.usage.completion},
                               {"cached", item.usage.cached}};
    return out;
}

std::pair<std::string, nlohmann::json> split_event(const agent::Event& event) {
    nlohmann::json json = agent::to_json(event);
    std::string kind = json.value("type", "");
    json.erase("type");
    return {std::move(kind), std::move(json)};
}

const agent::ToolOutput* raw_tool_output(const agent::Event& event) {
    if (const auto* direct = std::get_if<agent::ToolOutput>(&event)) return direct;
    if (const auto* sub = std::get_if<agent::SubEvent>(&event)) {
        return std::get_if<agent::ToolOutput>(&sub->event());
    }
    return nullptr;
}

protocol::RpcError to_rpc_error(const runtime::RuntimeError& error) {
    using Kind = runtime::RuntimeError::Kind;
    protocol::RpcError out;
    out.code = protocol::rpc_code::kBusinessError;
    out.message = error.message;
    switch (error.kind) {
    case Kind::busy: out.kind = protocol::error_kind::kBusy; break;
    case Kind::stale_session: out.kind = protocol::error_kind::kStaleSession; break;
    case Kind::not_found: out.kind = protocol::error_kind::kNotFound; break;
    case Kind::session_in_use: out.kind = protocol::error_kind::kSessionInUse; break;
    case Kind::invalid_state: out.kind = protocol::error_kind::kInvalidState; break;
    case Kind::config_error: out.kind = protocol::error_kind::kConfigError; break;
    case Kind::query_failed: out.kind = protocol::error_kind::kQueryFailed; break;
    case Kind::startup_failed: out.kind = protocol::error_kind::kStartupFailed; break;
    case Kind::closing: out.kind = protocol::error_kind::kClosing; break;
    case Kind::interaction_closed: out.kind = protocol::error_kind::kInteractionClosed; break;
    }
    return out;
}

} // namespace dagent::backend
