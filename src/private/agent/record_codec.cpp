#include "agent/record_codec.hpp"

#include <format>
#include <utility>

namespace dagent::agent::record_codec {
namespace {

using nlohmann::json;

[[noreturn]] void corrupt(std::string_view type, std::string_view message) {
    throw RecordError(RecordError::Kind::corrupt,
                      std::format("corrupt session record {}: {}", type, message));
}

void require_object(std::string_view type, const json& payload) {
    if (!payload.is_object()) corrupt(type, "payload must be an object");
}

std::string string_field(std::string_view type, const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) corrupt(type, std::format("field {} must be a string", key));
    return it->get<std::string>();
}

std::int64_t integer_field(std::string_view type, const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || (!it->is_number_integer() && !it->is_number_unsigned())) {
        corrupt(type, std::format("field {} must be an integer", key));
    }
    try {
        return it->get<std::int64_t>();
    } catch (const json::exception&) {
        corrupt(type, std::format("field {} is out of range", key));
    }
}

bool bool_field(std::string_view type, const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_boolean()) corrupt(type, std::format("field {} must be a boolean", key));
    return it->get<bool>();
}

Usage parse_usage(std::string_view type, const json& value) {
    if (!value.is_object()) corrupt(type, "field usage must be an object");
    Usage usage;
    usage.prompt = integer_field(type, value, "prompt");
    usage.completion = integer_field(type, value, "completion");
    usage.cached = integer_field(type, value, "cached");
    return usage;
}

TurnStatus parse_status(std::string_view value) {
    if (value == "done") return TurnStatus::done;
    if (value == "interrupted") return TurnStatus::interrupted;
    if (value == "denied") return TurnStatus::denied;
    if (value == "limit") return TurnStatus::limit;
    if (value == "failed" || value == "crashed") return TurnStatus::failed;
    corrupt("turn_end", std::format("unknown status {}", value));
}

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

Record system(std::string_view text, std::string_view model) {
    return {"system", json{{"schema", 1}, {"text", text}, {"model", model}}};
}

Record user(std::int64_t n, std::string_view text) {
    return {"user", json{{"n", n}, {"text", text}}};
}

Record assistant(std::int64_t n, const Reply& reply) {
    json tool_calls = json::array();
    for (const ToolCall& call : reply.message.tool_calls) {
        tool_calls.push_back({{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}});
    }
    json payload = {{"n", n},
                    {"content", reply.message.content},
                    {"reasoning", reply.message.reasoning_content},
                    {"reasoning_signature", reply.message.reasoning_signature},
                    {"tool_calls", std::move(tool_calls)},
                    {"finish", reply.finish.raw}};
    if (reply.usage) payload["usage"] = usage_json(*reply.usage);
    return {"assistant", std::move(payload)};
}

Record tool_started(const ToolStarted& event) {
    json payload = to_json(Event{event});
    payload.erase("type");
    payload["schema"] = 1;
    return {"tool_started", std::move(payload)};
}

Record tool(std::int64_t n, const ToolCall& call, std::string_view summary, const ToolResult& result) {
    return {"tool", json{{"n", n},
                         {"call_id", call.id},
                         {"name", call.name},
                         {"summary", summary},
                         {"text", result.model_text},
                         {"is_error", result.is_error},
                         {"interrupted", result.interrupted},
                         {"view", to_json(result.display)}}};
}

Record permission(const Approval& approval, const Decision& decision) {
    json requests = json::array();
    for (const auto& request : approval.requests) {
        const char* kind = "dynamic_command";
        switch (request.kind) {
        case Approval::Request::Kind::dynamic_command: break;
        case Approval::Request::Kind::read_path: kind = "read_path"; break;
        case Approval::Request::Kind::write_path: kind = "write_path"; break;
        case Approval::Request::Kind::network: kind = "network"; break;
        case Approval::Request::Kind::sensitive_read: kind = "sensitive_read"; break;
        case Approval::Request::Kind::protected_write: kind = "protected_write"; break;
        case Approval::Request::Kind::host_access: kind = "host_access"; break;
        }
        requests.push_back({{"kind", kind}, {"target", request.target}, {"reason", request.reason}});
    }
    return {"permission", json{{"schema", 2},
                               {"call_id", approval.call_id},
                               {"answer", answer_name(decision.answer)},
                               {"rule", approval.session_rule},
                               {"network", decision.network},
                               {"cwd", approval.cwd},
                               {"mode", approval.mode},
                               {"partially_executed", approval.partially_executed},
                               {"requests", std::move(requests)}}};
}

Record permission_revoked(std::string_view id) {
    return {"permission_revoked", json{{"schema", 1}, {"id", id}}};
}

Record prune(const std::vector<std::int64_t>& ordinals) {
    return {"prune", json{{"ordinals", ordinals}}};
}

Record compaction(std::int64_t keep_from, std::string_view summary) {
    return {"compaction", json{{"keep_from", keep_from}, {"summary", summary}}};
}

Record turn_end(TurnStatus status, std::string_view error, int steps, int tool_calls, const Usage& total) {
    return {"turn_end", json{{"status", to_string(status)},
                             {"error", error},
                             {"steps", steps},
                             {"tool_calls", tool_calls},
                             {"usage", usage_json(total)}}};
}

Record turn_end_crashed() {
    return {"turn_end", json{{"status", "crashed"},
                             {"error", ""},
                             {"steps", 0},
                             {"tool_calls", 0},
                             {"usage", usage_json(Usage{})}}};
}

// ---- 解码 ----

DecodedRecord decode(std::string_view type, const nlohmann::json& payload) {
    require_object(type, payload);

    if (type == "system") {
        if (integer_field(type, payload, "schema") != 1)
            corrupt(type, std::format("unknown session format version {}",
                                      integer_field(type, payload, "schema")));
        SystemRecord record;
        record.text = string_field(type, payload, "text");
        record.model = string_field(type, payload, "model");
        return record;
    }
    if (type == "user") {
        UserRecord record;
        record.n = integer_field(type, payload, "n");
        record.text = string_field(type, payload, "text");
        return record;
    }
    if (type == "assistant") {
        AssistantRecord record;
        record.n = integer_field(type, payload, "n");
        record.message.role = Role::assistant;
        record.message.content = string_field(type, payload, "content");
        record.message.reasoning_content = string_field(type, payload, "reasoning");
        record.message.reasoning_signature = string_field(type, payload, "reasoning_signature");
        const auto calls = payload.find("tool_calls");
        if (calls == payload.end() || !calls->is_array()) corrupt(type, "field tool_calls must be an array");
        for (const json& item : *calls) {
            if (!item.is_object()) corrupt(type, "tool_calls entries must be objects");
            record.message.tool_calls.push_back(ToolCall{string_field(type, item, "id"),
                                                         string_field(type, item, "name"),
                                                         string_field(type, item, "arguments")});
        }
        record.finish = string_field(type, payload, "finish");
        if (const auto usage = payload.find("usage"); usage != payload.end()) {
            record.usage = parse_usage(type, *usage);
        }
        return record;
    }
    if (type == "tool_started") {
        if (integer_field(type, payload, "schema") != 1) corrupt(type, "unknown schema");
        ToolStartedRecord record;
        ExecutionGrant& grant = record.event.grant;
        const std::string sandbox = string_field(type, payload, "sandbox");
        if (sandbox == "read_only") grant.sandbox = SandboxProfile::read_only;
        else if (sandbox == "workspace_write") grant.sandbox = SandboxProfile::workspace_write;
        else if (sandbox == "full_access") grant.sandbox = SandboxProfile::full_access;
        else corrupt(type, "unknown sandbox profile");
        grant.backend = string_field(type, payload, "backend");
        const std::string source = string_field(type, payload, "grant_source");
        if (source == "mode") grant.source = GrantSource::mode;
        else if (source == "once") grant.source = GrantSource::once;
        else if (source == "session") grant.source = GrantSource::session;
        else if (source == "unrestricted") grant.source = GrantSource::unrestricted;
        else corrupt(type, "unknown grant source");
        grant.analysis_version = static_cast<int>(integer_field(type, payload, "analysis_version"));
        grant.allow_network = bool_field(type, payload, "network");
        grant.allow_local_sockets = bool_field(type, payload, "local_sockets");
        grant.private_tmp = bool_field(type, payload, "private_tmp");
        grant.protect_sensitive_names = bool_field(type, payload, "protect_sensitive_names");
        const auto paths = [&](const char* key) {
            const auto found = payload.find(key);
            if (found == payload.end() || !found->is_array())
                corrupt(type, std::format("field {} must be an array", key));
            std::vector<std::filesystem::path> result;
            for (const auto& value : *found) {
                if (!value.is_string()) corrupt(type, std::format("field {} entries must be strings", key));
                result.emplace_back(value.get<std::string>());
            }
            return result;
        };
        grant.readable = paths("readable");
        grant.writable = paths("writable");
        grant.protected_read = paths("protected_read");
        grant.protected_write = paths("protected_write");
        const auto targets = payload.find("network_targets");
        if (targets == payload.end() || !targets->is_array())
            corrupt(type, "field network_targets must be an array");
        for (const auto& value : *targets) {
            if (!value.is_string()) corrupt(type, "network_targets entries must be strings");
            grant.network_targets.push_back(value.get<std::string>());
        }
        record.event.id = string_field(type, payload, "id");
        record.event.name = string_field(type, payload, "name");
        record.event.summary = string_field(type, payload, "summary");
        return record;
    }
    if (type == "tool") {
        ToolRecord record;
        record.n = integer_field(type, payload, "n");
        record.call.id = string_field(type, payload, "call_id");
        record.call.name = string_field(type, payload, "name");
        record.summary = string_field(type, payload, "summary");
        record.result.model_text = string_field(type, payload, "text");
        record.result.is_error = bool_field(type, payload, "is_error");
        record.result.interrupted = bool_field(type, payload, "interrupted");
        const auto view = payload.find("view");
        if (view == payload.end() || !view->is_object()) corrupt(type, "field view must be an object");
        record.result.display = view_from_json(*view);
        return record;
    }
    if (type == "permission") {
        if (integer_field(type, payload, "schema") != 2) corrupt(type, "unknown schema");
        (void)string_field(type, payload, "cwd");
        (void)string_field(type, payload, "mode");
        (void)bool_field(type, payload, "partially_executed");
        const auto requests = payload.find("requests");
        if (requests == payload.end() || !requests->is_array()) corrupt(type, "field requests must be an array");
        for (const json& request : *requests) {
            require_object(type, request);
            (void)string_field(type, request, "kind");
            (void)string_field(type, request, "target");
            (void)string_field(type, request, "reason");
        }
        PermissionRecord record;
        record.call_id = string_field(type, payload, "call_id");
        record.answer = string_field(type, payload, "answer");
        record.rule = string_field(type, payload, "rule");
        record.network = bool_field(type, payload, "network");
        return record;
    }
    if (type == "permission_revoked") {
        if (integer_field(type, payload, "schema") != 1) corrupt(type, "unknown schema");
        return PermissionRevokedRecord{string_field(type, payload, "id")};
    }
    if (type == "prune") {
        const auto ordinals = payload.find("ordinals");
        if (ordinals == payload.end() || !ordinals->is_array()) corrupt(type, "field ordinals must be an array");
        PruneRecord record;
        for (const auto& value : *ordinals) {
            if (!value.is_number_integer()) corrupt(type, "message ordinal must be an integer");
            record.ordinals.push_back(value.get<std::int64_t>());
        }
        return record;
    }
    if (type == "compaction") {
        CompactionRecord record;
        record.keep_from = integer_field(type, payload, "keep_from");
        record.summary = string_field(type, payload, "summary");
        return record;
    }
    if (type == "turn_end") {
        const std::string status_name = string_field(type, payload, "status");
        const std::string stored_error = string_field(type, payload, "error");
        const auto usage = payload.find("usage");
        if (usage == payload.end()) corrupt(type, "missing field usage");
        TurnEndRecord record;
        record.status = parse_status(status_name);
        record.error = status_name == "crashed" && stored_error.empty()
                           ? "session unexpectedly interrupted"
                           : stored_error;
        record.steps = static_cast<int>(integer_field(type, payload, "steps"));
        record.tool_calls = static_cast<int>(integer_field(type, payload, "tool_calls"));
        record.usage = parse_usage(type, *usage);
        return record;
    }
    corrupt(type, "unknown record type");
}

} // namespace dagent::agent::record_codec
