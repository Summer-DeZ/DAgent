#include "agent/record/record_codec.hpp"

#include <format>
#include <utility>

namespace dagent::agent::record_codec {
namespace {

using nlohmann::json;

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
    usage.prompt = static_cast<int>(integer_field(type, value, "prompt"));
    usage.completion = static_cast<int>(integer_field(type, value, "completion"));
    usage.cached = static_cast<int>(integer_field(type, value, "cached"));
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

ApprovalAuthority parse_authority(std::string_view type, const json& payload) {
    const std::string authority = string_field(type, payload, "authority");
    if (authority == "user") return ApprovalAuthority::user;
    if (authority == "parent_model") return ApprovalAuthority::parent_model;
    if (authority == "decision_model") return ApprovalAuthority::decision_model;
    corrupt(type, "unknown approval authority");
}

Approval::Scope parse_approval_scope(std::string_view type, std::string_view value) {
    if (value == "operation") return Approval::Scope::operation;
    if (value == "exact_file_read") return Approval::Scope::exact_file_read;
    if (value == "network_target") return Approval::Scope::network_target;
    if (value == "web_request") return Approval::Scope::web_request;
    if (value == "mcp_action") return Approval::Scope::mcp_action;
    corrupt(type, "unknown approval scope");
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

} // namespace

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
        if (const auto origin = payload.find("origin"); origin != payload.end() && !origin->is_null()) {
            if (!origin->is_string()) corrupt(type, "origin must be a string");
            const std::string value = origin->get<std::string>();
            if (value == "user") record.origin = UserInputOrigin::user;
            else if (value == "delegation") record.origin = UserInputOrigin::delegation;
            else corrupt(type, "unknown user origin");
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
        const int schema = static_cast<int>(integer_field(type, payload, "schema"));
        // schema 2 的 network 布尔已由显式请求范围取代；旧记录只读回放，忽略该字段。
        if (schema != 2 && schema != 3 && schema != 4 && schema != 5) corrupt(type, "unknown schema");
        if (type == "parent_review" && schema != 4 && schema != 5) corrupt(type, "unknown parent review schema");
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
            if (request["kind"] == "host_access" || request["kind"] == "host_command")
                approval.requests.push_back({request["kind"] == "host_access"
                                                 ? Approval::Request::Kind::host_access
                                                 : Approval::Request::Kind::host_command,
                    request["target"].get<std::string>(), request["reason"].get<std::string>()});
        }
        if (schema >= 4) {
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
        if (schema >= 5) {
            record.approval_scope = string_field(type, payload, "approval_scope");
            (void)parse_approval_scope(type, record.approval_scope);
            const auto review_ids = payload.find("review_ids");
            if (review_ids == payload.end() || !review_ids->is_array())
                corrupt(type, "review_ids must be an array");
            for (const auto& value : *review_ids) {
                if (!value.is_string()) corrupt(type, "review_ids entries must be strings");
                record.review_ids.push_back(value.get<std::string>());
            }
        } else {
            record.approval_scope = "operation";
        }
        record.audit = payload;
        record.summary = permission_summary(approval, decision);
        if (type == "parent_review") return ParentReviewRecord{std::move(record)};
        return record;
    }
    if (type == "auto_review") {
        if (integer_field(type, payload, "schema") != 1) corrupt(type, "unknown schema");
        AutoReviewRecord record;
        record.call_id = string_field(type, payload, "call_id");
        record.result = string_field(type, payload, "result");
        record.reason = string_field(type, payload, "reason");
        const auto review_ids = payload.find("review_ids");
        if (review_ids == payload.end() || !review_ids->is_array())
            corrupt(type, "review_ids must be an array");
        for (const auto& value : *review_ids) {
            if (!value.is_string()) corrupt(type, "review_ids entries must be strings");
            record.review_ids.push_back(value.get<std::string>());
        }
        return record;
    }
    if (type == "delegation_context") {
        if (integer_field(type, payload, "schema") != 1) corrupt(type, "unknown schema");
        DelegationContextRecord record;
        record.task = string_field(type, payload, "task");
        const auto messages = payload.find("user_messages");
        if (messages == payload.end() || !messages->is_array())
            corrupt(type, "user_messages must be an array");
        for (const auto& value : *messages) {
            if (!value.is_string()) corrupt(type, "user_messages entries must be strings");
            record.user_messages.push_back(value.get<std::string>());
        }
        return record;
    }
    if (type == "unrestricted_ack") {
        if (integer_field(type, payload, "schema") != 1) corrupt(type, "unknown schema");
        return UnrestrictedAckRecord{};
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
