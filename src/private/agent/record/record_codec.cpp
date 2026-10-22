#include "agent/record/record_codec.hpp"

#include <format>
#include <utility>

namespace dagent::agent::record_codec {
namespace {

using nlohmann::json;

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


std::string_view approval_scope_name(Approval::Scope scope) {
    switch (scope) {
    case Approval::Scope::operation: return "operation";
    case Approval::Scope::exact_file_read: return "exact_file_read";
    case Approval::Scope::network_target: return "network_target";
    case Approval::Scope::web_request: return "web_request";
    case Approval::Scope::mcp_action: return "mcp_action";
    }
    return "operation";
}


bool allowed(const Decision& decision) {
    return decision.answer == Decision::Answer::allow ||
           decision.answer == Decision::Answer::allow_session;
}

} // namespace

std::string decision_state(const Decision& decision) {
    return decision.state.empty() ? (allowed(decision) ? "approved" : "denied") : decision.state;
}

std::string decision_scope(const Approval& approval, const Decision& decision) {
    if (!allowed(decision)) return "none";
    if (decision.answer == Decision::Answer::allow) return "one_call";
    return approval.identity.parent_session_id.empty() ? "session" : "child_session";
}

Record system(std::string_view text, std::string_view model) {
    return {"system", json{{"schema", 1}, {"text", text}, {"model", model}}};
}

Record user(std::int64_t n, std::string_view text, const std::vector<SkillView>& skills,
            UserInputOrigin origin) {
    json payload{{"n", n}, {"text", text}, {"origin", origin == UserInputOrigin::user ? "user" : "delegation"}};
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
    const std::string_view source = approval.authority == ApprovalAuthority::parent_model ? "Parent model"
                                       : approval.authority == ApprovalAuthority::decision_model ? "Auto-review"
                                                                                                : "User";
    std::string text = std::format("{} permission {}: {} [{}]", source,
        decision_state(decision), approval.tool.empty() ? approval.call_id : approval.tool,
        decision_scope(approval, decision));
    if (!approval.agent.empty()) text += " via " + approval.agent;
    for (const auto& request : approval.requests) {
        if (request.kind == Approval::Request::Kind::host_access) {
            text += "; host full access";
            break;
        }
        if (request.kind == Approval::Request::Kind::host_command) {
            text += "; host command (no sandbox)";
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
        case Approval::Request::Kind::host_command: kind = "host_command"; break;
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
    json reviews = json::array();
    for (const std::string& id : approval.review_ids) reviews.push_back(id);
    json payload{{"schema", 5},
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
                               {"approval_scope", approval_scope_name(approval.scope)},
                               {"review_ids", std::move(reviews)},
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

Record auto_review(const AutoReview& event) {
    json reviews = json::array();
    for (const std::string& id : event.review_ids) reviews.push_back(id);
    return {"auto_review", json{{"schema", 1},
                                {"call_id", event.call_id},
                                {"review_ids", std::move(reviews)},
                                {"result", event.result},
                                {"reason", event.reason}}};
}

Record delegation_context(std::string_view task, const std::vector<std::string>& user_messages) {
    json messages = json::array();
    for (const std::string& message : user_messages) messages.push_back(message);
    return {"delegation_context", json{{"schema", 1}, {"task", task}, {"user_messages", std::move(messages)}}};
}

Record unrestricted_ack() {
    return {"unrestricted_ack", json{{"schema", 1}}};
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

} // namespace dagent::agent::record_codec
