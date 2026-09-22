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
    return path == root / "models.json" || path == root / "dagent.db" ||
           is_relative_to(path, root / "logs");
}

std::string exec_rule_id(std::string_view command, std::string_view cwd) {
    const std::size_t value = std::hash<std::string>{}(std::string(command) + '\0' + std::string(cwd));
    return std::format("exec-{:x}", value);
}

std::string read_rule_id(const fs::path& path) {
    return std::format("read-{:x}", std::hash<std::string>{}(path.string()));
}

bool single_use_only(const Approval& approval) {
    return std::ranges::any_of(approval.requests, [](const Approval::Request& request) {
        return request.kind == Approval::Request::Kind::sensitive_read ||
               request.kind == Approval::Request::Kind::protected_write ||
               request.kind == Approval::Request::Kind::host_access;
    });
}

} // namespace

std::string_view to_string(PermissionMode mode) {
    switch (mode) {
    case PermissionMode::ask: return "ask";
    case PermissionMode::workspace: return "workspace";
    case PermissionMode::unrestricted: return "unrestricted";
    }
    return "workspace";
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
    if (sensitive_control_path(path, control_root_)) return PathClass::guarded;
    for (const auto& part : path) {
        if (part == ".git") return PathClass::guarded;
        if (part == ".ssh" || part == ".gnupg") return PathClass::sensitive;
    }
    if (sensitive_name(path.filename().string())) return PathClass::sensitive;
    if (!intent.inside_workspace) return PathClass::outside;
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
        for (const auto& path : intent.paths)
            for (const auto& dir : read_dirs_)
                if (is_relative_to(path.path, dir)) return false;
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

ExecutionGrant Policy::grant_for_exec(SandboxProfile profile, GrantSource source) const {
    ExecutionGrant grant;
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
    grant.readable.insert(grant.readable.end(), sandbox_options_.extra_readable.begin(),
                          sandbox_options_.extra_readable.end());
    grant.readable.insert(grant.readable.end(), sandbox_options_.extra_writable.begin(),
                          sandbox_options_.extra_writable.end());
    if (profile == SandboxProfile::workspace_write) {
        grant.writable = {workspace_root_};
        grant.writable.insert(grant.writable.end(), sandbox_options_.extra_writable.begin(),
                              sandbox_options_.extra_writable.end());
    }
    grant.protected_read = {control_root_ / "models.json", control_root_ / "dagent.db",
                            control_root_ / "logs", workspace_root_ / ".env"};
    if (const char* home = std::getenv("HOME")) {
        grant.protected_read.emplace_back(fs::path(home) / ".ssh");
        grant.protected_read.emplace_back(fs::path(home) / ".gnupg");
    }
    grant.protected_write = {project_root_ / ".git"};
    grant.protected_write.insert(grant.protected_write.end(), grant.protected_read.begin(),
                                 grant.protected_read.end());
    return grant;
}

Verdict Policy::evaluate(const ToolCall& call, const PreparedIntent& intent) const {
    const std::lock_guard lock(rules_mutex_);
    Verdict verdict;
    verdict.approval.call_id = call.id;
    verdict.approval.tool = call.name;
    verdict.approval.intent = intent;
    verdict.approval.cwd = workspace_root_.string();
    verdict.approval.mode = planning() ? "plan" : std::string(to_string(mode()));

    const auto has = [&](PathClass cls) {
        return std::ranges::any_of(intent.paths, [&](const ResourceIntent& path) {
            return classify(path) == cls;
        });
    };
    const auto answer = [&](Verdict::Kind kind) {
        verdict.kind = kind;
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

    if (planning() || read_only()) {
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
            verdict.reason = planning()
                                 ? "Planning mode permits research only; state-changing and dynamic commands are disabled"
                                 : "read-only mode: writes, dynamic commands and external tools are disabled";
            return verdict;
        }
    }

    if (mode() == PermissionMode::unrestricted) {
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
                const std::string dir = outside->path.parent_path().string();
                verdict.approval.requests.push_back({Approval::Request::Kind::read_path, dir,
                                                     "read outside the workspace"});
                verdict.approval.session_rule = std::format("Allow reads under {} for this session", dir);
            }
            verdict.kind = Verdict::Kind::ask;
        } else return answer(Verdict::Kind::allow);
        break;
    case ToolKind::write:
        if (has(PathClass::guarded) || has(PathClass::sensitive)) {
            verdict.approval.reason = "Modify protected agent data or git internals";
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

    if (matches_session(verdict.approval, intent)) {
        if (intent.kind == ToolKind::exec)
            verdict.grant = grant_for_exec(SandboxProfile::workspace_write, GrantSource::session);
        return answer(Verdict::Kind::allow);
    }

    if (mode() == PermissionMode::workspace) {
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

ExecutionGrant Policy::grant_for(const Approval& approval, const Decision& decision) const {
    if (approval.intent.kind != ToolKind::exec) return {};
    if (std::ranges::any_of(approval.requests, [](const Approval::Request& request) {
            return request.kind == Approval::Request::Kind::host_access;
        }))
        return grant_for_exec(SandboxProfile::full_access, GrantSource::once);
    ExecutionGrant grant = grant_for_exec(SandboxProfile::workspace_write,
                                          decision.answer == Decision::Answer::allow_session
                                              ? GrantSource::session : GrantSource::once);
    grant.allow_network = false;
    return grant;
}

void Policy::remember(const Approval& approval, const Decision& decision) {
    if (decision.answer != Decision::Answer::allow_session || single_use_only(approval)) return;
    const std::lock_guard lock(rules_mutex_);
    switch (approval.intent.kind) {
    case ToolKind::write: session_edits_ = true; return;
    case ToolKind::read:
        for (const auto& path : approval.intent.paths)
            if (classify(path) == PathClass::outside) read_dirs_.push_back(path.path.parent_path());
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
    if (session_edits_) grants.push_back({"workspace-edits", "Workspace file edits"});
    for (const auto& path : read_dirs_)
        grants.push_back({read_rule_id(path), "Read under " + path.string()});
    for (const auto& tool : external_rules_) grants.push_back({"external-" + tool, "External tool " + tool});
    for (const auto& rule : exec_rules_) {
        std::string command = rule.command;
        if (command.size() > 80) command = command.substr(0, 77) + "...";
        grants.push_back({rule.id, "Command: " + command});
    }
    return grants;
}

bool Policy::revoke(std::string_view id) {
    const std::lock_guard lock(rules_mutex_);
    if (id == "workspace-edits" && session_edits_) { session_edits_ = false; return true; }
    if (id.starts_with("read-")) {
        const auto it = std::ranges::find_if(read_dirs_, [&](const fs::path& path) {
            return read_rule_id(path) == id;
        });
        if (it == read_dirs_.end()) return false;
        read_dirs_.erase(it);
        return true;
    }
    if (id.starts_with("external-")) {
        const auto it = std::ranges::find(external_rules_, id.substr(9));
        if (it == external_rules_.end()) return false;
        external_rules_.erase(it);
        return true;
    }
    const auto it = std::ranges::find_if(exec_rules_, [&](const ExecRule& rule) { return rule.id == id; });
    if (it == exec_rules_.end()) return false;
    exec_rules_.erase(it);
    return true;
}

void Policy::set_mode(PermissionMode mode) { mode_.store(mode); }
PermissionMode Policy::mode() const { return mode_.load(); }
void Policy::set_read_only(bool value) { read_only_.store(value); }
bool Policy::read_only() const { return read_only_.load(); }
void Policy::set_planning(bool value) { planning_.store(value); }
bool Policy::planning() const { return planning_.load(); }

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
