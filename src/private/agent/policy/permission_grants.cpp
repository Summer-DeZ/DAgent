#include "agent/policy/permission.hpp"

#include <format>
#include <functional>
#include <mutex>
#include <string_view>

#include "base/text.hpp"

namespace dagent::agent {
namespace {

namespace fs = std::filesystem;

std::string exec_rule_id(std::string_view command, std::string_view cwd) {
    const std::size_t value = std::hash<std::string>{}(std::string(command) + '\0' + std::string(cwd));
    return std::format("exec-{:x}", value);
}

std::string read_rule_id(const fs::path& path) {
    return std::format("read-{:x}", std::hash<std::string>{}(path.string()));
}

std::string read_file_rule_id(const fs::path& path) {
    return std::format("read-file-{:x}", std::hash<std::string>{}(path.string()));
}

std::string network_rule_id(std::string_view host, int port) {
    return std::format("net-{:x}", std::hash<std::string>{}(std::string(host) + ":" + std::to_string(port)));
}

std::string network_deny_id(std::string_view host, int port) {
    return std::format("netdeny-{:x}", std::hash<std::string>{}(std::string(host) + ":" + std::to_string(port)));
}

/// 配置里的目标模式：完整 `host:port`、裸 `host`，或前导 `*.` 的域名后缀。
bool network_pattern_matches(std::string_view pattern, const NetworkTarget& target) {
    if (pattern.empty()) return false;
    if (const auto colon = pattern.rfind(':'); colon != std::string_view::npos &&
                                                pattern.find_first_not_of("0123456789", colon + 1) == std::string_view::npos) {
        if (std::stoi(std::string(pattern.substr(colon + 1))) != target.port) return false;
        pattern = pattern.substr(0, colon);
    }
    if (pattern.starts_with("*.")) {
        const std::string_view suffix = pattern.substr(1); // ".example.com"
        return target.host.size() > suffix.size() && target.host.ends_with(suffix);
    }
    return pattern == target.host;
}

} // namespace

bool Policy::valid_approval(const Approval& approval, const Decision& decision) const {
    if (decision.answer != Decision::Answer::allow && decision.answer != Decision::Answer::allow_session)
        return false;
    if (decision.answer == Decision::Answer::allow_session &&
        (approval.session_rule.empty() || single_use_only(approval))) return false;
    if (approval.identity.child_revision != revision()) return false;
    if (approval.authority == ApprovalAuthority::decision_model) {
        // 决策模型只允许本计划列出的范围：网络目标只放行一次，工作区外文件可有文件级会话授权。
        if (approval.review_ids.empty()) return false;
        switch (approval.scope) {
        case Approval::Scope::network_target:
        case Approval::Scope::web_request:
        case Approval::Scope::mcp_action:
            return decision.answer == Decision::Answer::allow;
        case Approval::Scope::exact_file_read:
            return true;
        case Approval::Scope::operation:
            return false;
        }
    }
    if (approval.authority != ApprovalAuthority::parent_model) return true;
    const auto& id = approval.identity;
    return !id.request_id.empty() && !id.parent_session_id.empty() && !id.child_session_id.empty() &&
           !id.origin_call_id.empty() && id.call_id == approval.call_id &&
           approval.authority_stop.stop_possible() &&
           !approval.authority_stop.stop_requested();
}

std::optional<ExecutionGrant> Policy::consume_approval(const Approval& approval, const Decision& decision) {
    const std::lock_guard lock(rules_mutex_);
    if (!valid_approval(approval, decision)) return std::nullopt;
    remember_locked(approval, decision);
    return grant_for(approval, decision);
}

bool Policy::consume_network_approval(const NetworkTarget& target, const Approval& approval,
                                     const Decision& decision) {
    const std::lock_guard lock(rules_mutex_);
    if (!valid_approval(approval, decision)) return false;
    if (decision.answer == Decision::Answer::allow_session) remember_network_locked(target, approval);
    return true;
}

ExecutionGrant Policy::grant_for(const Approval& approval, const Decision& decision) const {
    const auto source = decision.answer == Decision::Answer::allow_session ? GrantSource::session : GrantSource::once;
    ExecutionGrant grant;
    if (approval.scope == Approval::Scope::exact_file_read) {
        // 只读精确文件：bash 保持只读 profile，仅追加被批准文件的读取范围。
        if (approval.intent.kind == ToolKind::exec) {
            grant = grant_for_exec(SandboxProfile::read_only, source);
            for (const auto& path : approval.intent.paths)
                if (!path.inside_workspace) grant.readable.push_back(path.path);
        } else {
            grant = grant_for_files(approval.intent, source);
        }
        grant.authority = approval.authority;
        grant.approval = approval.identity;
        grant.authority_stop = approval.authority_stop;
        return grant;
    }
    if (approval.intent.kind == ToolKind::read || approval.intent.kind == ToolKind::write)
        grant = grant_for_files(approval.intent, source);
    else if (approval.intent.kind == ToolKind::external)
        grant = grant_for_external(approval.intent, source);
    else if (approval.intent.kind == ToolKind::exec) {
        const bool host_access = std::ranges::any_of(approval.requests, [](const Approval::Request& request) {
            return request.kind == Approval::Request::Kind::host_access;
        });
        const bool host_command = std::ranges::any_of(approval.requests, [](const Approval::Request& request) {
            return request.kind == Approval::Request::Kind::host_command;
        });
        const bool host = host_access || host_command;
        const bool readonly = approval.intent.command && approval.intent.command->known_readonly;
        grant = grant_for_exec(host ? SandboxProfile::full_access
                                   : readonly ? SandboxProfile::read_only : SandboxProfile::workspace_write,
                               host_access ? GrantSource::once : source);
        if (!host && readonly) {
            for (const auto& path : approval.intent.paths) {
                grant.readable.push_back(path.path);
                if (classify(path) == PathClass::sensitive || classify(path) == PathClass::guarded)
                    grant.read_exceptions.push_back(path.path);
            }
        }
    }
    grant.authority = approval.authority;
    grant.approval = approval.identity;
    grant.authority_stop = approval.authority_stop;
    return grant;
}

void Policy::remember_locked(const Approval& approval, const Decision& decision) {
    if (decision.answer != Decision::Answer::allow_session || single_use_only(approval)) return;
    revision_.fetch_add(1);
    if (approval.authority == ApprovalAuthority::parent_model) {
        parent_grants_.push_back({approval, decision});
        return;
    }
    if (approval.authority == ApprovalAuthority::decision_model ||
        approval.scope == Approval::Scope::exact_file_read) {
        // 精确文件读取（无论决策模型还是人工批准）只保存文件级规则；网络与 MCP 不生成会话规则。
        if (approval.scope != Approval::Scope::exact_file_read) return;
        for (const auto& path : approval.intent.paths) {
            if (path.inside_workspace) continue;
            const std::string id = read_file_rule_id(path.path);
            if (std::ranges::none_of(read_files_, [&](const FileRule& rule) { return rule.id == id; }))
                read_files_.push_back({id, path.path, approval.identity, approval.review_ids});
        }
        return;
    }
    switch (approval.intent.kind) {
    case ToolKind::memory:
    case ToolKind::network: return;
    case ToolKind::write: session_edits_ = true; return;
    case ToolKind::read:
        for (const auto& path : approval.intent.paths)
            if (classify(path) == PathClass::outside) read_dirs_.push_back(path.path);
        return;
    case ToolKind::external: external_rules_.push_back(approval.tool); return;
    case ToolKind::exec: {
        if (!approval.intent.command) return;
        const std::string id = exec_rule_id(approval.intent.command->command, workspace_root_.string());
        if (std::ranges::none_of(exec_rules_, [&](const ExecRule& rule) { return rule.id == id; }))
            exec_rules_.push_back({id, approval.intent.command->command, workspace_root_.string()});
        return;
    }
    }
}

std::vector<Policy::SessionGrant> Policy::session_grants() const {
    const std::lock_guard lock(rules_mutex_);
    std::vector<SessionGrant> grants;
    for (const auto& saved : parent_grants_)
        if (!saved.approval.authority_stop.stop_requested())
            grants.push_back({saved.approval.identity.request_id, "Parent model: " + saved.approval.session_rule});
    for (const auto& rule : parent_network_rules_)
        if (!rule.approval.authority_stop.stop_requested())
            grants.push_back({rule.approval.identity.request_id, "Parent model: " + rule.approval.session_rule});
    if (session_edits_) grants.push_back({"workspace-edits", "Workspace file edits"});
    for (const auto& path : read_dirs_)
        grants.push_back({read_rule_id(path), "Read under " + path.string()});
    for (const auto& rule : read_files_)
        grants.push_back({rule.id, "Read file " + rule.path.string()});
    for (const auto& rule : network_rules_)
        grants.push_back({rule.id, "Network " + rule.host + ":" + std::to_string(rule.port)});
    for (const auto& rule : network_denied_)
        grants.push_back({rule.id, "Denied network " + rule.host + ":" + std::to_string(rule.port)});
    for (const auto& tool : external_rules_) grants.push_back({"external-" + tool, "External tool " + tool});
    for (const auto& rule : exec_rules_) {
        std::string command = rule.command;
        if (command.size() > 80) command = command.substr(0, base::utf8_floor(command, 77)) + "...";
        grants.push_back({rule.id, "Command: " + command});
    }
    return grants;
}

bool Policy::revoke(std::string_view id) {
    std::unique_lock lock(rules_mutex_);
    const auto changed = [&] {
        revision_.fetch_add(1);
        auto previous = std::move(authority_stop_);
        authority_stop_ = std::stop_source{};
        lock.unlock();
        previous.request_stop();
        return true;
    };
    if (std::erase_if(parent_grants_, [&](const RememberedApproval& saved) {
            return saved.approval.identity.request_id == id;
        }) != 0) { return changed(); }
    if (std::erase_if(parent_network_rules_, [&](const ParentNetworkRule& rule) {
            return rule.approval.identity.request_id == id;
        }) != 0) return changed();
    if (id.starts_with("netdeny-")) {
        const auto it = std::ranges::find_if(network_denied_, [&](const NetworkRule& rule) { return rule.id == id; });
        if (it == network_denied_.end()) return false;
        network_denied_.erase(it);
        return changed();
    }
    if (id.starts_with("net-")) {
        const auto it = std::ranges::find_if(network_rules_, [&](const NetworkRule& rule) { return rule.id == id; });
        if (it == network_rules_.end()) return false;
        network_rules_.erase(it);
        return changed();
    }
    if (id == "workspace-edits" && session_edits_) { session_edits_ = false; return changed(); }
    if (id.starts_with("read-file-")) {
        const auto it = std::ranges::find_if(read_files_, [&](const FileRule& rule) { return rule.id == id; });
        if (it == read_files_.end()) return false;
        read_files_.erase(it);
        return changed();
    }
    if (id.starts_with("read-")) {
        const auto it = std::ranges::find_if(read_dirs_, [&](const fs::path& path) {
            return read_rule_id(path) == id;
        });
        if (it == read_dirs_.end()) return false;
        read_dirs_.erase(it);
        return changed();
    }
    if (id.starts_with("external-")) {
        const auto it = std::ranges::find(external_rules_, id.substr(9));
        if (it == external_rules_.end()) return false;
        external_rules_.erase(it);
        return changed();
    }
    const auto it = std::ranges::find_if(exec_rules_, [&](const ExecRule& rule) { return rule.id == id; });
    if (it == exec_rules_.end()) return false;
    exec_rules_.erase(it);
    return changed();
}

std::uint64_t Policy::revision() const { return revision_.load(); }

Policy::NetworkRules Policy::inspect_network(const NetworkTarget& target) const {
    const std::lock_guard lock(rules_mutex_);
    NetworkRules rules;
    rules.revision = revision();
    for (const auto& pattern : sandbox_options_.network_denied)
        if (network_pattern_matches(pattern, target)) {
            rules.match = NetworkRules::Match::denied;
            rules.reason = "network target is on the configured deny list";
            return rules;
        }
    for (const auto& rule : network_denied_)
        if (rule.host == target.host && rule.port == target.port) {
            rules.match = NetworkRules::Match::denied;
            rules.reason = rule.authority == ApprovalAuthority::parent_model
                               ? "the parent agent denied this network target earlier in the session"
                               : "the user denied this network target earlier in the session";
            return rules;
        }
    const auto live = effective();
    if (target.infrastructure || (live.mode == PermissionMode::unrestricted && !live.read_only && !live.planning)) {
        rules.match = NetworkRules::Match::allowed;
        return rules;
    }
    for (const auto& rule : parent_network_rules_)
        if (!rule.approval.authority_stop.stop_requested() && rule.target.host == target.host &&
            rule.target.port == target.port) {
            rules.match = NetworkRules::Match::allowed;
            return rules;
        }
    for (const auto& rule : network_rules_)
        if (rule.host == target.host && rule.port == target.port) {
            rules.match = NetworkRules::Match::allowed;
            return rules;
        }
    for (const auto& pattern : sandbox_options_.network_allowed)
        if (network_pattern_matches(pattern, target)) {
            rules.match = NetworkRules::Match::allowed;
            return rules;
        }
    rules.reason = std::format("connect to {}:{} from a sandboxed command", target.host, target.port);
    return rules;
}

Policy::NetworkDecision Policy::check_network(const NetworkTarget& target) const {
    const NetworkRules rules = inspect_network(target);
    switch (rules.match) {
    case NetworkRules::Match::denied: return {NetworkDecision::Kind::deny, rules.reason};
    case NetworkRules::Match::allowed: return {NetworkDecision::Kind::allow, {}};
    case NetworkRules::Match::unmatched: break;
    }
    return {NetworkDecision::Kind::ask, rules.reason};
}

void Policy::remember_network_locked(const NetworkTarget& target, const Approval& approval) {
    if (approval.authority == ApprovalAuthority::parent_model) {
        parent_network_rules_.push_back({target, approval});
        revision_.fetch_add(1);
        return;
    }
    const std::string id = network_rule_id(target.host, target.port);
    if (std::ranges::none_of(network_rules_, [&](const NetworkRule& rule) { return rule.id == id; }))
        network_rules_.push_back({id, target.host, target.port});
    revision_.fetch_add(1);
}

void Policy::remember_denied_network(const NetworkTarget& target, ApprovalAuthority authority) {
    const std::lock_guard lock(rules_mutex_);
    const std::string id = network_deny_id(target.host, target.port);
    if (std::ranges::none_of(network_denied_, [&](const NetworkRule& rule) { return rule.id == id; }))
        network_denied_.push_back({id, target.host, target.port, authority});
    revision_.fetch_add(1);
}

} // namespace dagent::agent
