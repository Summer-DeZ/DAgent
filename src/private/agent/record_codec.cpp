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

ApprovalAuthority parse_authority(std::string_view type, const json& payload) {
    const std::string authority = string_field(type, payload, "authority");
    if (authority == "user") return ApprovalAuthority::user;
    if (authority == "parent_model") return ApprovalAuthority::parent_model;
    corrupt(type, "unknown approval authority");
}

ApprovalIdentity parse_identity(std::string_view type, const json& value) {
    require_object(type, value);
    ApprovalIdentity identity;
    identity.request_id = string_field(type, value, "request_id");
    identity.parent_session_id = string_field(type, value, "parent_session_id");
    identity.child_session_id = string_field(type, value, "child_session_id");
    identity.origin_call_id = string_field(type, value, "origin_call_id");
    identity.call_id = string_field(type, value, "call_id");
    identity.execution_id = string_field(type, value, "execution_id");
    identity.parent_revision = integer_field(type, value, "parent_revision");
    identity.child_revision = integer_field(type, value, "child_revision");
    return identity;
}

bool allowed(const Decision& decision) {
    return decision.answer == Decision::Answer::allow ||
           decision.answer == Decision::Answer::allow_session;
}

std::string decision_state(const Decision& decision) {
    return decision.state.empty() ? (allowed(decision) ? "approved" : "denied") : decision.state;
}

std::string decision_scope(const Approval& approval, const Decision& decision) {
    if (!allowed(decision)) return "none";
    if (decision.answer == Decision::Answer::allow) return "one_call";
    return approval.identity.child_session_id.empty() ? "session" : "child_session";
}

} // namespace

Record system(std::string_view text, std::string_view model) {
    return {"system", json{{"schema", 1}, {"text", text}, {"model", model}}};
}

Record user(std::int64_t n, std::string_view text, const std::vector<SkillView>& skills) {
    json payload{{"n", n}, {"text", text}};
    if (!skills.empty()) {
        payload["skills"] = json::array();
        for (const auto& skill : skills)
            payload["skills"].push_back({{"name", skill.name}, {"path", skill.path}});
    }
    return {"user", std::move(payload)};
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
    payload["schema"] = 3;
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

std::string permission_summary(const Approval& approval, const Decision& decision) {
    std::string text = std::format("{} permission {}: {} [{}]",
        approval.authority == ApprovalAuthority::parent_model ? "Parent model" : "User",
        decision_state(decision), approval.tool.empty() ? approval.call_id : approval.tool,
        decision_scope(approval, decision));
    if (!approval.agent.empty()) text += " via " + approval.agent;
    for (const auto& request : approval.requests) {
        if (request.kind == Approval::Request::Kind::host_access) {
            text += "; host full access";
            break;
        }
    }
    if (!decision.feedback.empty()) text += ": " + decision.feedback;
    return text;
}

Record permission(const Approval& approval, const Decision& decision, const ExecutionGrant* grant) {
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
    json paths = json::array();
    for (const auto& path : approval.intent.paths)
        paths.push_back({{"path", path.path.string()},
                         {"access", path.access == Access::read ? "read" : "write"},
                         {"inside_workspace", path.inside_workspace}});
    json intent{{"summary", approval.intent.summary}, {"preview", approval.intent.preview},
                {"paths", std::move(paths)}, {"external_boundary", approval.intent.external_boundary}};
    if (approval.intent.command) {
        const auto& command = *approval.intent.command;
        json impacts = json::array();
        for (const auto& impact : command.impacts) {
            const char* kind = impact.kind == ImpactKind::read ? "read"
                             : impact.kind == ImpactKind::write ? "write"
                             : impact.kind == ImpactKind::network ? "network" : "special";
            impacts.push_back({{"kind", kind}, {"target", impact.target},
                               {"reason", impact.reason}, {"dynamic", impact.dynamic}});
        }
        intent["command"] = {{"command", command.command}, {"analysis_version", command.analysis_version},
                              {"dynamic", command.dynamic}, {"known_readonly", command.known_readonly},
                              {"dangerous", command.dangerous}, {"impacts", std::move(impacts)}};
    }
    const auto& identity = approval.identity;
    json payload{{"schema", 4},
                               {"call_id", approval.call_id},
                               {"answer", answer_name(decision.answer)},
                               {"rule", approval.session_rule},
                               {"cwd", approval.cwd},
                               {"mode", approval.mode},
                               {"read_only", approval.read_only}, {"planning", approval.planning},
                               {"existing_permissions", approval.existing_permissions},
                               {"partially_executed", approval.partially_executed},
                               {"requests", requests},
                               {"authority", to_string(approval.authority)},
                               {"identity", {{"request_id", identity.request_id},
                                              {"parent_session_id", identity.parent_session_id},
                                              {"child_session_id", identity.child_session_id},
                                              {"origin_call_id", identity.origin_call_id},
                                              {"call_id", identity.call_id},
                                              {"execution_id", identity.execution_id},
                                              {"parent_revision", identity.parent_revision},
                                              {"child_revision", identity.child_revision}}},
                               {"tool", approval.tool}, {"arguments", approval.arguments},
                               {"agent", approval.agent}, {"origin_call_id", approval.origin_call_id},
                               {"delegated_task", approval.delegated_task}, {"intent", std::move(intent)},
                               {"request_reason", approval.reason},
                               {"requested_scope", approval.session_rule.empty() ? "one_call" : "one_call_or_session"},
                               {"state", decision_state(decision)},
                               {"explicit_denial", decision.explicit_denial},
                               {"scope", decision_scope(approval, decision)},
                               {"approved_requests", allowed(decision) ? requests : json::array()},
                               {"reason", decision.feedback}, {"model", decision.model},
                               {"usage", usage_json(decision.usage)},
                               {"estimated_budget_tokens", decision.estimated_budget_tokens},
                               {"usage_owner", approval.authority == ApprovalAuthority::parent_model
                                                   ? identity.parent_session_id : std::string{}},
                               {"actual_grant", grant ? tool_started(ToolStarted{approval.call_id, approval.tool,
                                                              approval.intent.summary, *grant}).payload : json(nullptr)}};
    return {"permission", std::move(payload)};
}

Record parent_review(const Approval& approval, const Decision& decision) {
    Record record = permission(approval, decision);
    record.type = "parent_review";
    return record;
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
        if (const auto skills = payload.find("skills"); skills != payload.end()) {
            if (!skills->is_array()) corrupt(type, "skills must be an array");
            for (const auto& skill : *skills) {
                require_object(type, skill);
                record.skills.push_back({string_field(type, skill, "name"), string_field(type, skill, "path")});
            }
        }
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
        const auto schema = integer_field(type, payload, "schema");
        if (schema != 1 && schema != 2 && schema != 3) corrupt(type, "unknown schema");
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
        if (schema >= 2) {
            grant.revision = integer_field(type, payload, "revision");
            grant.read_exceptions = paths("read_exceptions");
        }
        if (schema >= 3) {
            if (payload.contains("execution_id")) grant.execution_id = string_field(type, payload, "execution_id");
            grant.authority = parse_authority(type, payload);
            const auto identity = payload.find("approval");
            if (identity == payload.end()) corrupt(type, "missing approval identity");
            grant.approval = parse_identity(type, *identity);
        }
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
    if (type == "permission" || type == "parent_review") {
        const int schema = integer_field(type, payload, "schema");
        // schema 2 的 network 布尔已由显式请求范围取代；旧记录只读回放，忽略该字段。
        if (schema != 2 && schema != 3 && schema != 4) corrupt(type, "unknown schema");
        if (type == "parent_review" && schema != 4) corrupt(type, "unknown parent review schema");
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
        Approval approval;
        approval.call_id = record.call_id;
        Decision decision;
        if (record.answer == "allow") decision.answer = Decision::Answer::allow;
        else if (record.answer == "allow_session") decision.answer = Decision::Answer::allow_session;
        else if (record.answer == "deny_with_feedback") decision.answer = Decision::Answer::deny_with_feedback;
        else if (record.answer != "deny") corrupt(type, "unknown approval answer");
        for (const auto& request : *requests) {
            if (request["kind"] == "host_access")
                approval.requests.push_back({Approval::Request::Kind::host_access,
                    request["target"].get<std::string>(), request["reason"].get<std::string>()});
        }
        if (schema == 4) {
            record.authority = parse_authority(type, payload);
            const auto identity = payload.find("identity");
            if (identity == payload.end()) corrupt(type, "missing approval identity");
            record.identity = parse_identity(type, *identity);
            record.state = string_field(type, payload, "state");
            if (record.state != "approved" && record.state != "denied" &&
                record.state != "cancelled" && record.state != "expired")
                corrupt(type, "unknown approval state");
            record.scope = string_field(type, payload, "scope");
            if (record.scope != "one_call" && record.scope != "session" &&
                record.scope != "child_session" && record.scope != "none")
                corrupt(type, "unknown approval scope");
            record.reason = string_field(type, payload, "reason");
            record.model = string_field(type, payload, "model");
            record.tool = string_field(type, payload, "tool");
            const auto usage = payload.find("usage");
            if (usage == payload.end()) corrupt(type, "missing field usage");
            record.usage = parse_usage(type, *usage);
            const auto approved_requests = payload.find("approved_requests");
            if (approved_requests == payload.end() || !approved_requests->is_array())
                corrupt(type, "approved_requests must be an array");
            for (const auto& request : *approved_requests) {
                require_object(type, request);
                (void)string_field(type, request, "kind");
                (void)string_field(type, request, "target");
                (void)string_field(type, request, "reason");
            }
            const auto actual_grant = payload.find("actual_grant");
            if (actual_grant == payload.end()) corrupt(type, "missing field actual_grant");
            if (!actual_grant->is_null()) (void)decode("tool_started", *actual_grant);
            approval.authority = record.authority;
            approval.identity = record.identity;
            approval.tool = record.tool;
            approval.agent = string_field(type, payload, "agent");
            decision.state = record.state;
            decision.feedback = record.reason;
        } else {
            record.state = decision_state(decision);
            record.scope = decision_scope(approval, decision);
        }
        record.audit = payload;
        record.summary = permission_summary(approval, decision);
        if (type == "parent_review") return ParentReviewRecord{std::move(record)};
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
