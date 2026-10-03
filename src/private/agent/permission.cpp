#include "agent/permission.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <functional>
#include <optional>
#include <string_view>

namespace dagent::agent {
namespace {

namespace fs = std::filesystem;

bool starts_with(std::string_view text, std::string_view prefix) { return text.starts_with(prefix); }
bool ends_with(std::string_view text, std::string_view suffix) { return text.ends_with(suffix); }

bool sensitive_name(std::string_view name) {
    return name == ".env" || starts_with(name, ".env.") || ends_with(name, ".pem") ||
           ends_with(name, ".key") || starts_with(name, "id_rsa") || starts_with(name, "id_ed25519");
}

bool is_relative_to(const fs::path& path, const fs::path& base) {
    const fs::path relative = path.lexically_relative(base);
    if (relative.empty()) return false;
    return relative.begin() == relative.end() || *relative.begin() != "..";
}

bool sensitive_control_path(const fs::path& path, const fs::path& root) {
    return !root.empty() && (is_relative_to(path, root / "config") || is_relative_to(path, root / "data") ||
           is_relative_to(path, root / "logs") || is_relative_to(path, root / "run") ||
           is_relative_to(path, root / "runtime"));
}

std::string exec_rule_id(std::string_view command, std::string_view cwd) {
    const std::size_t value = std::hash<std::string>{}(std::string(command) + '\0' + std::string(cwd));
    return std::format("exec-{:x}", value);
}

std::string read_rule_id(const fs::path& path) {
    return std::format("read-{:x}", std::hash<std::string>{}(path.string()));
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

int mode_rank(PermissionMode mode) { return static_cast<int>(mode); }

PermissionMode stricter_mode(PermissionMode a, PermissionMode b) {
    return mode_rank(a) <= mode_rank(b) ? a : b;
}

bool single_use_only(const Approval& approval) {
    return std::ranges::any_of(approval.requests, [](const Approval::Request& request) {
        return request.kind == Approval::Request::Kind::sensitive_read ||
               request.kind == Approval::Request::Kind::protected_write ||
               request.kind == Approval::Request::Kind::host_access;
    });
}

} // namespace

ResourceClass classify_resource(const fs::path& path, bool inside_workspace, const fs::path& control_root) {
    if (sensitive_control_path(path, control_root)) return ResourceClass::guarded;
    for (const auto& part : path) {
        if (part == ".git") return ResourceClass::guarded;
        if (part == ".ssh" || part == ".gnupg") return ResourceClass::sensitive;
    }
    if (sensitive_name(path.filename().string())) return ResourceClass::sensitive;
    if (!inside_workspace) return ResourceClass::outside;
    return ResourceClass::normal;
}

std::string_view to_string(PermissionMode mode) {
    switch (mode) {
    case PermissionMode::ask: return "ask";
    case PermissionMode::workspace: return "workspace";
    case PermissionMode::unrestricted: return "unrestricted";
    }
    return "workspace";
}

std::string_view to_string(ApprovalAuthority authority) {
    return authority == ApprovalAuthority::parent_model ? "parent_model" : "user";
}

std::string_view to_string(SandboxProfile profile) {
    switch (profile) {
    case SandboxProfile::read_only: return "read_only";
    case SandboxProfile::workspace_write: return "workspace_write";
    case SandboxProfile::full_access: return "full_access";
    }
    return "workspace_write";
}

std::string_view to_string(GrantSource source) {
    switch (source) {
    case GrantSource::mode: return "mode";
    case GrantSource::once: return "once";
    case GrantSource::session: return "session";
    case GrantSource::unrestricted: return "unrestricted";
    }
    return "mode";
}

Policy::Policy(PermissionMode mode, bool read_only, bool planning, SandboxSupport sandbox,
               SandboxConfig sandbox_options, int analysis_version,
               fs::path workspace_root, fs::path project_root, fs::path control_root)
    : mode_(mode), read_only_(read_only || planning), planning_(planning), sandbox_(std::move(sandbox)),
      sandbox_options_(std::move(sandbox_options)), analysis_version_(analysis_version),
      workspace_root_(std::move(workspace_root)), project_root_(std::move(project_root)),
      control_root_(std::move(control_root)) {}

bool Policy::inside_dir(const fs::path& path, const fs::path& dir) const {
    return !dir.empty() && is_relative_to(path, dir);
}

Policy::PathClass Policy::classify(const ResourceIntent& intent) const {
    const fs::path& path = intent.path;
    const auto category = classify_resource(path, intent.inside_workspace, control_root_);
    if (inside_dir(path, control_root_ / "runtime")) return PathClass::guarded;
    if (category == ResourceClass::guarded) return PathClass::guarded;
    if (category == ResourceClass::sensitive) return PathClass::sensitive;
    const auto& extra = intent.access == Access::read ? sandbox_options_.extra_readable
                                                     : sandbox_options_.extra_writable;
    for (const auto& directory : extra)
        if (inside_dir(path, directory)) return PathClass::normal;
    if (intent.access == Access::read)
        for (const auto& directory : sandbox_options_.extra_writable)
            if (inside_dir(path, directory)) return PathClass::normal;
    if (intent.access == Access::read)
        for (const auto& directory : sandbox_options_.skill_readable)
            if (inside_dir(path, directory)) return PathClass::normal;
    if (category == ResourceClass::outside) return PathClass::outside;
    return PathClass::normal;
}

std::optional<bool> Policy::matches_session(const Approval& approval, const PreparedIntent& intent) const {
    switch (intent.kind) {
    case ToolKind::write:
        if (session_edits_ && std::ranges::all_of(intent.paths, [&](const ResourceIntent& path) {
                return classify(path) == PathClass::normal;
            })) return false;
        return std::nullopt;
    case ToolKind::read:
        if (std::ranges::all_of(intent.paths, [&](const ResourceIntent& path) {
                return classify(path) == PathClass::normal ||
                       std::ranges::any_of(read_dirs_, [&](const fs::path& dir) {
                           return is_relative_to(path.path, dir);
                       });
            })) return false;
        return std::nullopt;
    case ToolKind::external:
        return std::ranges::find(external_rules_, approval.tool) != external_rules_.end()
                   ? std::optional<bool>{false} : std::nullopt;
    case ToolKind::exec: {
        if (!intent.command) return std::nullopt;
        const auto rule = std::ranges::find_if(exec_rules_, [&](const ExecRule& item) {
            return item.command == intent.command->command && item.cwd == workspace_root_.string();
        });
        return rule == exec_rules_.end() ? std::nullopt : std::optional<bool>{false};
    }
    }
    return std::nullopt;
}

bool Policy::matches_parent_session(const Approval& saved, const Approval& requested) const {
    if (saved.intent.kind != requested.intent.kind) return false;
    switch (requested.intent.kind) {
    case ToolKind::write:
        return std::ranges::all_of(requested.intent.paths, [&](const ResourceIntent& path) {
            return classify(path) == PathClass::normal;
        });
    case ToolKind::read:
        return std::ranges::all_of(requested.intent.paths, [&](const ResourceIntent& path) {
            return classify(path) == PathClass::normal ||
                   std::ranges::any_of(saved.intent.paths, [&](const ResourceIntent& allowed) {
                       return classify(allowed) == PathClass::outside && is_relative_to(path.path, allowed.path);
                   });
        });
    case ToolKind::external:
        return saved.tool == requested.tool;
    case ToolKind::exec:
        return saved.intent.command && requested.intent.command && saved.cwd == requested.cwd &&
               saved.intent.command->command == requested.intent.command->command;
    }
    return false;
}

ExecutionGrant Policy::grant_for_exec(SandboxProfile profile, GrantSource source) const {
    ExecutionGrant grant;
    grant.revision = revision();
    grant.sandbox = profile;
    grant.source = source;
    grant.backend = profile == SandboxProfile::full_access ? "host" : sandbox_.backend;
    grant.analysis_version = analysis_version_;
    grant.private_tmp = profile != SandboxProfile::full_access;
    grant.protect_sensitive_names = profile != SandboxProfile::full_access;
    if (profile == SandboxProfile::full_access) {
        grant.allow_network = true;
        grant.allow_local_sockets = true;
        return grant;
    }

    grant.readable = {workspace_root_};
    grant.readable.insert(grant.readable.end(), sandbox_options_.runtime_readable.begin(), sandbox_options_.runtime_readable.end());
    grant.readable.insert(grant.readable.end(), sandbox_options_.skill_readable.begin(),
                          sandbox_options_.skill_readable.end());
    grant.readable.insert(grant.readable.end(), sandbox_options_.extra_readable.begin(),
                          sandbox_options_.extra_readable.end());
    grant.readable.insert(grant.readable.end(), sandbox_options_.extra_writable.begin(),
                          sandbox_options_.extra_writable.end());
    if (profile == SandboxProfile::workspace_write) {
        grant.writable = {workspace_root_};
        grant.writable.insert(grant.writable.end(), sandbox_options_.extra_writable.begin(),
                              sandbox_options_.extra_writable.end());
    }
    grant.protected_read = {control_root_ / "config", control_root_ / "data",
                            control_root_ / "logs", control_root_ / "run", workspace_root_ / ".env"};
    if (const char* home = std::getenv("HOME")) {
        grant.protected_read.emplace_back(fs::path(home) / ".ssh");
        grant.protected_read.emplace_back(fs::path(home) / ".gnupg");
    }
    grant.protected_write = {project_root_ / ".git", control_root_ / "runtime"};
    grant.protected_write.insert(grant.protected_write.end(), grant.protected_read.begin(),
                                 grant.protected_read.end());
    return grant;
}

ExecutionGrant Policy::grant_for_external(const PreparedIntent& intent, GrantSource source) const {
    ExecutionGrant grant;
    grant.revision = revision();
    grant.source = source;
    grant.backend = intent.external_boundary.empty() ? "external" : intent.external_boundary;
    return grant;
}

ExecutionGrant Policy::grant_for_files(const PreparedIntent& intent, GrantSource source) const {
    ExecutionGrant grant;
    grant.revision = revision();
    grant.source = source;
    grant.backend = "native";
    const EffectivePermission live = effective();
    grant.protect_sensitive_names =
        live.mode != PermissionMode::unrestricted || live.read_only || live.planning;
    for (const auto& path : intent.paths) {
        if (path.access == Access::write) grant.writable.push_back(path.path);
        else {
            grant.readable.push_back(path.path);
            if (classify(path) == PathClass::sensitive || classify(path) == PathClass::guarded)
                grant.read_exceptions.push_back(path.path);
        }
    }
    return grant;
}

Verdict Policy::evaluate(const ToolCall& call, const PreparedIntent& intent) const {
    const std::lock_guard lock(rules_mutex_);
    const EffectivePermission live = effective();
    Verdict verdict;
    verdict.approval.identity.child_revision = revision();
    verdict.approval.call_id = call.id;
    verdict.approval.tool = call.name;
    verdict.approval.arguments = call.arguments;
    verdict.approval.intent = intent;
    verdict.approval.cwd = workspace_root_.string();
    verdict.approval.mode = live.planning ? "plan" : std::string(to_string(live.mode));

    const auto has = [&](PathClass cls) {
        return std::ranges::any_of(intent.paths, [&](const ResourceIntent& path) {
            return classify(path) == cls;
        });
    };
    const auto answer = [&](Verdict::Kind kind) {
        verdict.kind = kind;
        const GrantSource source = live.mode == PermissionMode::unrestricted && !live.read_only && !live.planning
                                       ? GrantSource::unrestricted : GrantSource::mode;
        if (kind != Verdict::Kind::allow) return verdict;
        if (intent.kind == ToolKind::read || intent.kind == ToolKind::write)
            verdict.grant = grant_for_files(intent, source);
        else if (intent.kind == ToolKind::external)
            verdict.grant = grant_for_external(intent, source);
        return verdict;
    };

    if (intent.kind == ToolKind::exec && intent.command && intent.command->dangerous) {
        verdict.kind = Verdict::Kind::deny;
        verdict.reason = "dangerous system-level command blocked by policy";
        return verdict;
    }
    if (intent.kind == ToolKind::exec && intent.command && intent.command->syntax != SyntaxState::valid) {
        verdict.kind = Verdict::Kind::deny;
        verdict.reason = "bash syntax is invalid or unsupported; command not executed";
        return verdict;
    }

    if (live.planning || live.read_only) {
        if (intent.kind == ToolKind::exec && intent.command && intent.command->known_readonly) {
            if (!sandbox_.read_only_ready) {
                verdict.kind = Verdict::Kind::deny;
                verdict.reason = "required read-only sandbox capabilities are unavailable";
                return verdict;
            }
            verdict.grant = grant_for_exec(SandboxProfile::read_only, GrantSource::mode);
            return answer(Verdict::Kind::allow);
        }
        if (intent.kind != ToolKind::read) {
            verdict.kind = Verdict::Kind::deny;
            verdict.reason = live.planning
                                 ? "Planning mode permits research only; state-changing and dynamic commands are disabled"
                                 : "read-only mode: writes, dynamic commands and external tools are disabled";
            return verdict;
        }
    }

    if (live.mode == PermissionMode::unrestricted && !live.read_only && !live.planning) {
        if (intent.kind == ToolKind::exec)
            verdict.grant = grant_for_exec(SandboxProfile::full_access, GrantSource::unrestricted);
        return answer(Verdict::Kind::allow);
    }

    switch (intent.kind) {
    case ToolKind::read:
        if (has(PathClass::sensitive) || has(PathClass::guarded)) {
            verdict.approval.reason = "Read protected agent data or a file that may contain secrets";
            for (const auto& path : intent.paths) {
                const PathClass cls = classify(path);
                if (cls == PathClass::sensitive || cls == PathClass::guarded)
                    verdict.approval.requests.push_back({Approval::Request::Kind::sensitive_read,
                                                         path.path.string(),
                                                         "protected data requires one-time approval"});
            }
            verdict.kind = Verdict::Kind::ask;
        } else if (has(PathClass::outside)) {
            verdict.approval.reason = "Read a file outside the workspace";
            const auto outside = std::ranges::find_if(intent.paths, [&](const ResourceIntent& path) {
                return classify(path) == PathClass::outside;
            });
            if (outside != intent.paths.end()) {
                const std::string dir = outside->path.string();
                verdict.approval.requests.push_back({Approval::Request::Kind::read_path, dir,
                                                     "read outside the workspace"});
                verdict.approval.session_rule = std::format("Allow reads under {} for this session", dir);
            }
            verdict.kind = Verdict::Kind::ask;
        } else return answer(Verdict::Kind::allow);
        break;
    case ToolKind::write:
        if (has(PathClass::guarded)) {
            verdict.reason = "execution control files require an explicit host maintenance operation";
            return answer(Verdict::Kind::deny);
        }
        if (has(PathClass::sensitive)) {
            verdict.approval.reason = "Modify sensitive user data";
            for (const auto& path : intent.paths) {
                const PathClass cls = classify(path);
                if (cls == PathClass::sensitive || cls == PathClass::guarded)
                    verdict.approval.requests.push_back({Approval::Request::Kind::protected_write,
                                                         path.path.string(),
                                                         "protected writes require one-time approval"});
            }
            verdict.kind = Verdict::Kind::ask;
        } else if (has(PathClass::outside)) {
            verdict.approval.reason = "Modify a file outside the workspace";
            for (const auto& path : intent.paths)
                if (classify(path) == PathClass::outside)
                    verdict.approval.requests.push_back({Approval::Request::Kind::write_path,
                                                         path.path.string(),
                                                         "write outside the workspace"});
            verdict.kind = Verdict::Kind::ask;
        } else {
            verdict.approval.reason = "Modify a file";
            verdict.approval.session_rule = "Allow edits inside the workspace for this session";
            verdict.kind = Verdict::Kind::ask;
        }
        break;
    case ToolKind::exec: {
        if (!intent.command) break;
        const CommandIntent& cmd = *intent.command;
        if (cmd.known_readonly) {
            if (!sandbox_.read_only_ready) {
                verdict.kind = Verdict::Kind::deny;
                verdict.reason = "required read-only sandbox capabilities are unavailable";
                return verdict;
            }
            verdict.grant = grant_for_exec(SandboxProfile::read_only, GrantSource::mode);
            return answer(Verdict::Kind::allow);
        }
        if (!sandbox_.workspace_ready) {
            verdict.approval.reason =
                "The workspace sandbox is unavailable; running this command requires one-time full host access";
            verdict.approval.requests.push_back({
                Approval::Request::Kind::host_access,
                cmd.command,
                "unsandboxed host access can reach the network, protected data, and git internals",
            });
            verdict.kind = Verdict::Kind::ask;
            return verdict;
        }
        verdict.approval.reason = cmd.dynamic
                                      ? "Run a dynamic command inside the workspace sandbox"
                                      : "Run a state-changing command inside the workspace sandbox";
        verdict.approval.requests.push_back({Approval::Request::Kind::dynamic_command, cmd.command,
                                             cmd.dynamic ? "runtime behavior cannot be fully resolved"
                                                         : "command may change workspace files"});
        verdict.approval.session_rule = "Allow this exact command in this workspace for this session";
        verdict.kind = Verdict::Kind::ask;
        break;
    }
    case ToolKind::external:
        verdict.approval.reason = std::format("Call external tool {}", call.name);
        verdict.approval.session_rule = std::format("Allow {} for this session", call.name);
        verdict.kind = Verdict::Kind::ask;
        break;
    }

    if (verdict.kind != Verdict::Kind::ask) return verdict;
    if (single_use_only(verdict.approval)) verdict.approval.session_rule.clear();

    if (!single_use_only(verdict.approval) && !verdict.approval.session_rule.empty()) {
        for (const auto& saved : parent_grants_) {
            if (saved.approval.authority_stop.stop_requested() ||
                !matches_parent_session(saved.approval, verdict.approval)) continue;
            verdict.kind = Verdict::Kind::allow;
            Approval current = verdict.approval;
            current.authority = saved.approval.authority;
            // Preserve the original approval identity; the Dispatcher identifies each consuming execution.
            current.identity = saved.approval.identity;
            current.authority_stop = saved.approval.authority_stop;
            verdict.grant = grant_for(current, saved.decision);
            return verdict;
        }
    }
    if (!single_use_only(verdict.approval) && matches_session(verdict.approval, intent)) {
        verdict.kind = Verdict::Kind::allow;
        if (intent.kind == ToolKind::exec)
            verdict.grant = grant_for_exec(SandboxProfile::workspace_write, GrantSource::session);
        else if (intent.kind == ToolKind::external)
            verdict.grant = grant_for_external(intent, GrantSource::session);
        else verdict.grant = grant_for_files(intent, GrantSource::session);
        return verdict;
    }

    if (live.mode == PermissionMode::workspace) {
        if (intent.kind == ToolKind::write && !has(PathClass::guarded) &&
            !has(PathClass::sensitive) && !has(PathClass::outside))
            return answer(Verdict::Kind::allow);
        if (intent.kind == ToolKind::exec) {
            verdict.grant = grant_for_exec(SandboxProfile::workspace_write, GrantSource::mode);
            return answer(Verdict::Kind::allow);
        }
    }
    return verdict;
}

bool Policy::valid_approval(const Approval& approval, const Decision& decision) const {
    if (decision.answer != Decision::Answer::allow && decision.answer != Decision::Answer::allow_session)
        return false;
    if (decision.answer == Decision::Answer::allow_session &&
        (approval.session_rule.empty() || single_use_only(approval))) return false;
    if (approval.identity.child_revision != revision()) return false;
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
    if (approval.intent.kind == ToolKind::read || approval.intent.kind == ToolKind::write)
        grant = grant_for_files(approval.intent, source);
    else if (approval.intent.kind == ToolKind::external)
        grant = grant_for_external(approval.intent, source);
    else if (approval.intent.kind == ToolKind::exec) {
        const bool host = std::ranges::any_of(approval.requests, [](const Approval::Request& request) {
            return request.kind == Approval::Request::Kind::host_access;
        });
        grant = grant_for_exec(host ? SandboxProfile::full_access : SandboxProfile::workspace_write,
                               host ? GrantSource::once : source);
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
    switch (approval.intent.kind) {
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
    for (const auto& rule : network_rules_)
        grants.push_back({rule.id, "Network " + rule.host + ":" + std::to_string(rule.port)});
    for (const auto& rule : network_denied_)
        grants.push_back({rule.id, "Denied network " + rule.host + ":" + std::to_string(rule.port)});
    for (const auto& tool : external_rules_) grants.push_back({"external-" + tool, "External tool " + tool});
    for (const auto& rule : exec_rules_) {
        std::string command = rule.command;
        if (command.size() > 80) command = command.substr(0, 77) + "...";
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

Policy::NetworkDecision Policy::check_network(const NetworkTarget& target) const {
    const std::lock_guard lock(rules_mutex_);
    for (const auto& pattern : sandbox_options_.network_denied)
        if (network_pattern_matches(pattern, target))
            return {NetworkDecision::Kind::deny, "network target is on the configured deny list"};
    for (const auto& rule : network_denied_)
        if (rule.host == target.host && rule.port == target.port)
            return {NetworkDecision::Kind::deny,
                    rule.authority == ApprovalAuthority::parent_model
                        ? "the parent agent denied this network target earlier in the session"
                        : "the user denied this network target earlier in the session"};
    for (const auto& rule : parent_network_rules_)
        if (!rule.approval.authority_stop.stop_requested() && rule.target.host == target.host && rule.target.port == target.port)
            return {NetworkDecision::Kind::allow, {}};
    for (const auto& rule : network_rules_)
        if (rule.host == target.host && rule.port == target.port) return {NetworkDecision::Kind::allow, {}};
    for (const auto& pattern : sandbox_options_.network_allowed)
        if (network_pattern_matches(pattern, target)) return {NetworkDecision::Kind::allow, {}};
    return {NetworkDecision::Kind::ask,
            std::format("connect to {}:{} from a sandboxed command", target.host, target.port)};
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

Policy::AuthorityView Policy::authority_view() const {
    const std::lock_guard lock(rules_mutex_);
    return {effective(), revision(), authority_stop_.get_token()};
}

void Policy::change_permission(const std::function<void()>& change) {
    std::stop_source previous;
    {
        const std::lock_guard lock(rules_mutex_);
        change();
        revision_.fetch_add(1);
        previous = std::move(authority_stop_);
        authority_stop_ = std::stop_source{};
    }
    // Cancellation callbacks may join executors; never call them while holding the Policy lock.
    previous.request_stop();
}

void Policy::set_mode(PermissionMode mode) { change_permission([&] { mode_.store(mode); }); }
PermissionMode Policy::mode() const { return mode_.load(); }
void Policy::set_read_only(bool value) { change_permission([&] { read_only_.store(value); }); }
bool Policy::read_only() const { return read_only_.load(); }
void Policy::set_planning(bool value) { change_permission([&] { planning_.store(value); }); }
bool Policy::planning() const { return planning_.load(); }

bool narrower(const EffectivePermission& a, const EffectivePermission& b) {
    if (a.planning != b.planning) return a.planning;
    if (a.read_only != b.read_only) return a.read_only;
    return mode_rank(a.mode) < mode_rank(b.mode);
}

EffectivePermission Policy::effective() const {
    EffectivePermission out;
    out.mode = mode();
    out.read_only = read_only();
    out.planning = planning();
    if (!cap_active_.load()) return out;
    out.mode = stricter_mode(out.mode, cap_mode_.load());
    out.read_only = out.read_only || cap_read_only_.load();
    out.planning = out.planning || cap_planning_.load();
    return out;
}

EffectivePermission Policy::set_parent_cap(PermissionMode mode, bool read_only, bool planning) {
    change_permission([&] {
        cap_mode_.store(mode);
        cap_read_only_.store(read_only);
        cap_planning_.store(planning);
        cap_active_.store(true);
    });
    return effective();
}

bool parallel(const Verdict& verdict, const PreparedIntent& intent) {
    if (verdict.kind != Verdict::Kind::allow) return false;
    if (intent.kind == ToolKind::read) return true;
    return intent.kind == ToolKind::exec && intent.command && intent.command->known_readonly &&
           verdict.grant.sandbox == SandboxProfile::read_only;
}

DerivedPermission derive_permission(PermissionMode parent_mode, bool parent_planning,
                                    bool parent_read_only, std::string_view def_permission) {
    DerivedPermission out;
    if (parent_planning || parent_read_only) {
        out.mode = PermissionMode::workspace; // read_only 下模式只影响 workspace 之外的读取
        out.read_only = true;
        out.planning = parent_planning;
        out.may_ask = false;
        return out;
    }
    // unrestricted 不继承：用户给 unrestricted 是针对自己盯着的这个会话，不是对自主运行的子 Agent 的授权。
    out.mode = parent_mode == PermissionMode::unrestricted ? PermissionMode::workspace : parent_mode;
    out.may_ask = true;
    if (def_permission == "read_only") {
        out.read_only = true;
    } else if (def_permission == "ask") {
        out.mode = PermissionMode::ask;
    }
    return out;
}

} // namespace dagent::agent
