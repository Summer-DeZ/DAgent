#include "agent/policy/permission.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <functional>
#include <optional>
#include <string_view>

#include "base/platform.hpp"
#include "base/path.hpp"

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
           is_relative_to(path, root / "runtime") || is_relative_to(path, root / "memory"));
}

int mode_rank(PermissionMode mode) { return static_cast<int>(mode); }

PermissionMode stricter_mode(PermissionMode a, PermissionMode b) {
    return mode_rank(a) <= mode_rank(b) ? a : b;
}

} // namespace

bool Policy::single_use_only(const Approval& approval) {
    return std::ranges::any_of(approval.requests, [](const Approval::Request& request) {
        return request.kind == Approval::Request::Kind::sensitive_read ||
               request.kind == Approval::Request::Kind::protected_write ||
               request.kind == Approval::Request::Kind::host_access;
    });
}

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

PermissionMode supported_mode(PermissionMode mode, bool workspace_available) {
    return mode == PermissionMode::workspace && !workspace_available ? PermissionMode::ask : mode;
}

std::string_view to_string(ApprovalAuthority authority) {
    switch (authority) {
    case ApprovalAuthority::parent_model: return "parent_model";
    case ApprovalAuthority::decision_model: return "decision_model";
    case ApprovalAuthority::user: return "user";
    }
    return "user";
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
    : mode_(supported_mode(mode, sandbox.workspace_ready)), read_only_(read_only || planning),
      planning_(planning), sandbox_(std::move(sandbox)),
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
    case ToolKind::memory:
    case ToolKind::network: return std::nullopt;
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
                       }) ||
                       (!path.directory && std::ranges::any_of(read_files_, [&](const FileRule& rule) {
                           return rule.path == path.path;
                       }));
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
        if (rule != exec_rules_.end()) return false;
        // 简单只读命令的工作区外读取：文件级授权可逐文件复用。
        if (intent.command->known_readonly) {
            const auto allowed_file = [&](const fs::path& path) {
                return std::ranges::any_of(read_files_, [&](const FileRule& item) {
                    return item.path == path;
                });
            };
            if (!intent.paths.empty() &&
                std::ranges::all_of(intent.paths, [&](const ResourceIntent& path) {
                    return classify(path) == PathClass::normal || (!path.directory && allowed_file(path.path));
                })) return false;
        }
        return std::nullopt;
    }
    }
    return std::nullopt;
}

bool Policy::matches_parent_session(const Approval& saved, const Approval& requested) const {
    if (saved.intent.kind != requested.intent.kind) return false;
    switch (requested.intent.kind) {
    case ToolKind::memory:
    case ToolKind::network: return false;
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
                            control_root_ / "logs", control_root_ / "run", control_root_ / "memory", workspace_root_ / ".env"};
    if (const auto home = base::home_directory()) {
        grant.protected_read.emplace_back(base::path_from_utf8(*home) / ".ssh");
        grant.protected_read.emplace_back(base::path_from_utf8(*home) / ".gnupg");
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
    grant.backend = intent.sandboxed_read ? sandbox_.backend : "native";
    if (intent.sandboxed_read) grant.sandbox = SandboxProfile::read_only;
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

    if (intent.kind == ToolKind::memory) return answer(Verdict::Kind::allow);

    // Network tools always enter read-only SRT, including unrestricted mode.
    // Destination authorization remains in the shared runtime network gate.
    if (intent.kind == ToolKind::network) {
        if (!sandbox_.read_only_ready) {
            verdict.reason = "required read-only SRT capabilities are unavailable; run dagent sandbox status";
            return verdict;
        }
        verdict.grant = grant_for_exec(SandboxProfile::read_only,
            live.mode == PermissionMode::unrestricted && !live.read_only && !live.planning
                ? GrantSource::unrestricted : GrantSource::mode);
        return answer(Verdict::Kind::allow);
    }

    if (live.planning || live.read_only) {
        if (intent.kind == ToolKind::exec && intent.command && intent.command->known_readonly) {
            if (!sandbox_.read_only_ready) {
                verdict.kind = Verdict::Kind::deny;
                verdict.reason = "planning and read-only modes run commands only inside a read-only sandbox; none is available";
                return verdict;
            }
        } else if (intent.kind != ToolKind::read) {
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
    case ToolKind::memory:
    case ToolKind::network: break; // handled above
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
            // 普通文件读取精确到文件；目录读取继续按目录范围审批（计划 §8.1）。
            const bool files_only = std::ranges::all_of(intent.paths, [&](const ResourceIntent& path) {
                return classify(path) != PathClass::outside || !path.directory;
            });
            verdict.approval.reason = files_only ? "Read a file outside the workspace"
                                                 : "Read outside the workspace";
            if (files_only) verdict.approval.scope = Approval::Scope::exact_file_read;
            std::size_t files = 0;
            for (const auto& path : intent.paths) {
                if (classify(path) != PathClass::outside) continue;
                ++files;
                verdict.approval.requests.push_back({Approval::Request::Kind::read_path,
                                                     path.path.string(),
                                                     files_only ? "read an outside file"
                                                                : "read outside the workspace"});
            }
            if (files_only)
                verdict.approval.session_rule = files == 1
                    ? "Allow reading this file for this session"
                    : "Allow reading these files for this session";
            else if (const auto outside = std::ranges::find_if(intent.paths, [&](const ResourceIntent& path) {
                         return classify(path) == PathClass::outside;
                     }); outside != intent.paths.end())
                verdict.approval.session_rule = std::format("Allow reads under {} for this session", outside->path.string());
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
        if (!sandbox_.workspace_ready) {
            verdict.approval.requests.push_back({
                Approval::Request::Kind::host_command, cmd.command,
                "runs directly on the host with your account's full access; no OS isolation",
            });
            bool protected_path = false;
            for (const auto& path : intent.paths) {
                const PathClass cls = classify(path);
                if (cls == PathClass::sensitive || cls == PathClass::guarded) {
                    protected_path = true;
                    verdict.approval.requests.push_back({Approval::Request::Kind::sensitive_read,
                                                         path.path.string(),
                                                         "protected data requires one-time approval"});
                }
            }
            verdict.approval.reason = cmd.known_readonly ? "Run a read-only command on the host"
                                    : cmd.dynamic       ? "Run a dynamic command on the host"
                                                        : "Run a state-changing command on the host";
            if (cmd.known_readonly && !protected_path)
                verdict.approval.session_rule = "Allow this exact command in this workspace for this session";
            verdict.kind = Verdict::Kind::ask;
            break;
        }
        if (cmd.known_readonly) {
            // 简单只读命令的工作区外普通文件读取：先按读取审批，不直接放行（计划 §2.3/§8.1）。
            std::vector<ResourceIntent> outside;
            bool protected_path = false;
            for (const auto& item : intent.paths) {
                const PathClass cls = classify(item);
                if (cls == PathClass::normal) continue;
                if (cls == PathClass::sensitive || cls == PathClass::guarded) protected_path = true;
                outside.push_back(item);
            }
            if (!outside.empty() || protected_path) {
                verdict.approval.intent = intent;
                verdict.approval.intent.paths = std::move(outside);
                if (protected_path) {
                    verdict.approval.reason = "Read a file outside the workspace that may contain secrets";
                    for (const auto& path : verdict.approval.intent.paths)
                        verdict.approval.requests.push_back(
                            {Approval::Request::Kind::sensitive_read, path.path.string(),
                             "protected data requires one-time approval"});
                } else {
                    verdict.approval.reason = "Read a file outside the workspace";
                    const bool files_only = std::ranges::none_of(verdict.approval.intent.paths,
                        [](const ResourceIntent& path) { return path.directory; });
                    if (files_only) verdict.approval.scope = Approval::Scope::exact_file_read;
                    for (const auto& path : verdict.approval.intent.paths)
                        verdict.approval.requests.push_back({Approval::Request::Kind::read_path,
                                                             path.path.string(), "read an outside file"});
                    verdict.approval.session_rule = !files_only ? "Allow this exact read command for this session"
                        : verdict.approval.intent.paths.size() == 1
                        ? "Allow reading this file for this session"
                        : "Allow reading these files for this session";
                }
                verdict.kind = Verdict::Kind::ask;
                break;
            }
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
        if (intent.kind == ToolKind::exec) {
            if (!sandbox_.workspace_ready) {
                verdict.grant = grant_for_exec(SandboxProfile::full_access, GrantSource::session);
            } else {
                const bool readonly = intent.command && intent.command->known_readonly;
                verdict.grant = grant_for_exec(readonly ? SandboxProfile::read_only : SandboxProfile::workspace_write,
                                               GrantSource::session);
                if (readonly && intent.command)
                    for (const auto& path : intent.command->read_paths)
                        if (!is_relative_to(path, workspace_root_)) verdict.grant.readable.push_back(path);
            }
        }
        else if (intent.kind == ToolKind::external)
            verdict.grant = grant_for_external(intent, GrantSource::session);
        else verdict.grant = grant_for_files(intent, GrantSource::session);
        return verdict;
    }

    if (live.mode == PermissionMode::workspace) {
        if (intent.kind == ToolKind::write && !has(PathClass::guarded) &&
            !has(PathClass::sensitive) && !has(PathClass::outside))
            return answer(Verdict::Kind::allow);
        // 仅自动批准工作区命令本身，不能覆盖上面生成的文件读取审批。
        if (intent.kind == ToolKind::exec &&
            std::ranges::any_of(verdict.approval.requests, [](const Approval::Request& request) {
                return request.kind == Approval::Request::Kind::dynamic_command;
            })) {
            verdict.grant = grant_for_exec(SandboxProfile::workspace_write, GrantSource::mode);
            return answer(Verdict::Kind::allow);
        }
    }
    return verdict;
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

void Policy::set_mode(PermissionMode mode) {
    change_permission([&] { mode_.store(supported_mode(mode, sandbox_.workspace_ready)); });
}
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
        cap_mode_.store(supported_mode(mode, sandbox_.workspace_ready));
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
