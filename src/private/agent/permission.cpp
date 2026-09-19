#include "agent/permission.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <optional>
#include <string_view>

#include "base/log.hpp"
#include "exec/shell.hpp"

namespace dagent::agent {
namespace {

namespace fs = std::filesystem;

std::shared_ptr<spdlog::logger> log_agent() { return base::logger("agent"); }

bool starts_with(std::string_view text, std::string_view prefix) { return text.starts_with(prefix); }

bool ends_with(std::string_view text, std::string_view suffix) { return text.ends_with(suffix); }

// 文件名（不含目录）匹配敏感模式。
bool sensitive_name(std::string_view name) {
    if (name == ".env") return true;
    if (starts_with(name, ".env.")) return true;
    if (ends_with(name, ".pem") || ends_with(name, ".key")) return true;
    if (starts_with(name, "id_rsa") || starts_with(name, "id_ed25519")) return true;
    return false;
}

bool is_relative_to(const fs::path& path, const fs::path& base) {
    const fs::path relative = path.lexically_relative(base);
    if (relative.empty()) return false;
    const std::string first = relative.begin() == relative.end() ? "" : relative.begin()->string();
    return first != "..";
}

// prefix(simple_command)：argv[0]，再加上第一个不以 - 开头的参数。
std::optional<std::string> command_prefix(const exec::SimpleCommand& command) {
    if (command.argv.empty() || command.argv.front().empty()) return std::nullopt;
    std::string prefix = command.argv.front();
    for (std::size_t i = 1; i < command.argv.size(); ++i) {
        if (!command.argv[i].empty() && command.argv[i].front() != '-') {
            prefix += ' ';
            prefix += command.argv[i];
            break;
        }
    }
    return prefix;
}

bool single_readonly(const exec::SimpleCommand& command) {
    exec::Analysis analysis;
    analysis.commands.push_back(command);
    return exec::is_known_readonly(analysis);
}

} // namespace

std::string_view to_string(PermissionMode mode) {
    switch (mode) {
    case PermissionMode::ask: return "ask";
    case PermissionMode::accept_edits: return "accept_edits";
    case PermissionMode::automatic: return "auto";
    case PermissionMode::deny: return "deny";
    }
    return "auto";
}

Policy::Policy(PermissionMode mode, exec::Support sandbox, fs::path workspace_root, fs::path project_root)
    : mode_(mode), sandbox_(sandbox), workspace_root_(std::move(workspace_root)),
      project_root_(std::move(project_root)) {}

bool Policy::inside_dir(const fs::path& path, const fs::path& dir) const {
    if (dir.empty()) return false;
    return is_relative_to(path, dir);
}

Policy::PathClass Policy::classify(const workspace::Resolved& resolved) const {
    const fs::path& path = resolved.path;
    if (inside_dir(path, project_root_ / ".dagent") || path.filename() == ".mcp.json") return PathClass::guarded;
    for (const auto& part : path) {
        if (part == ".git") return PathClass::guarded;
        if (part == ".ssh" || part == ".gnupg") return PathClass::sensitive;
    }
    if (sensitive_name(path.filename().string())) return PathClass::sensitive;
    if (!resolved.inside_workspace) return PathClass::outside;
    return PathClass::normal;
}

bool Policy::matches_session(const Approval& approval, const tools::Intent& intent) const {
    switch (intent.kind) {
    case tools::Intent::Kind::write:
        if (!session_edits_) return false;
        // 会话授权只覆盖工作区内的普通文件；受保护文件与工作区外永远要用户亲自确认。
        return std::all_of(intent.paths.begin(), intent.paths.end(), [&](const workspace::Resolved& path) {
            return classify(path) == PathClass::normal;
        });
    case tools::Intent::Kind::read:
        for (const auto& path : intent.paths) {
            for (const auto& dir : read_dirs_) {
                if (is_relative_to(path.path, dir)) return true;
            }
        }
        return false;
    case tools::Intent::Kind::external:
        return std::find(external_rules_.begin(), external_rules_.end(), approval.tool) !=
               external_rules_.end();
    case tools::Intent::Kind::exec: {
        const exec::Analysis analysis = exec::analyze(intent.command);
        if (analysis.has_opaque) return false;
        for (const auto& command : analysis.commands) {
            if (single_readonly(command)) continue;
            const auto prefix = command_prefix(command);
            if (!prefix) return false;
            const bool known = std::any_of(exec_rules_.begin(), exec_rules_.end(),
                                           [&](const ExecRule& rule) { return rule.prefix == *prefix; });
            if (!known) return false;
        }
        return true;
    }
    }
    return false;
}

Verdict Policy::evaluate(const ToolCall& call, const tools::Intent& intent) const {
    Verdict verdict;
    verdict.approval.call_id = call.id;
    verdict.approval.tool = call.name;
    verdict.approval.intent = intent;
    verdict.grant = tools::Grant{};

    const auto has = [&](PathClass cls) {
        return std::any_of(intent.paths.begin(), intent.paths.end(),
                           [&](const workspace::Resolved& path) { return classify(path) == cls; });
    };
    const auto answer = [&](Verdict::Kind kind) {
        verdict.kind = kind;
        return verdict;
    };

    switch (intent.kind) {
    case tools::Intent::Kind::read:
        if (has(PathClass::sensitive)) {
            verdict.approval.reason = "读取可能含密钥的文件";
            verdict.kind = Verdict::Kind::ask;
        } else if (has(PathClass::outside)) {
            verdict.approval.reason = "读取工作区外的文件";
            const auto outside = std::find_if(intent.paths.begin(), intent.paths.end(),
                                              [&](const workspace::Resolved& path) {
                                                  return classify(path) == PathClass::outside;
                                              });
            if (outside != intent.paths.end()) {
                verdict.approval.session_rule =
                    std::format("本会话内读取 {} 下的文件不再询问", outside->path.parent_path().string());
            }
            verdict.kind = Verdict::Kind::ask;
        } else {
            return answer(Verdict::Kind::allow);
        }
        break;
    case tools::Intent::Kind::write:
        if (has(PathClass::guarded)) {
            verdict.approval.reason = "修改 agent 配置 / git 内部文件";
            verdict.kind = Verdict::Kind::ask;
        } else if (has(PathClass::outside)) {
            verdict.approval.reason = "修改工作区外的文件";
            verdict.kind = Verdict::Kind::ask;
        } else {
            verdict.approval.reason = "修改文件";
            verdict.approval.session_rule = "本会话内修改工作区文件不再询问";
            verdict.kind = Verdict::Kind::ask;
        }
        break;
    case tools::Intent::Kind::exec: {
        const bool sandbox = sandbox_.landlock_abi > 0 && sandbox_.seccomp;
        if (!sandbox) {
            verdict.approval.reason = "当前系统不支持沙箱，命令会不受限制地运行";
            verdict.kind = Verdict::Kind::ask;
        } else if (intent.known_readonly) {
            verdict.grant.sandbox = exec::Mode::read_only;
            verdict.grant.allow_network = false;
            return answer(Verdict::Kind::allow);
        } else {
            verdict.approval.reason = "运行命令";
            verdict.approval.can_network = true;
            verdict.approval.session_rule = "本会话内运行相同的命令不再询问";
            verdict.kind = Verdict::Kind::ask;
        }
        break;
    }
    case tools::Intent::Kind::external:
        verdict.approval.reason = std::format("调用外部工具 {}", call.name);
        verdict.approval.session_rule = std::format("本会话内调用 {} 不再询问", call.name);
        verdict.kind = Verdict::Kind::ask;
        break;
    }

    if (verdict.kind != Verdict::Kind::ask) return verdict;

    // 步骤 3：本会话授权。
    if (matches_session(verdict.approval, intent)) return answer(Verdict::Kind::allow);

    // 步骤 4：按模式转换剩下的 ask。
    switch (mode()) {
    case PermissionMode::ask:
        return verdict;
    case PermissionMode::accept_edits:
        if (intent.kind == tools::Intent::Kind::write && !has(PathClass::guarded) &&
            !has(PathClass::outside)) {
            return answer(Verdict::Kind::allow);
        }
        return verdict;
    case PermissionMode::automatic:
        if (intent.kind == tools::Intent::Kind::write &&
            (has(PathClass::guarded) || has(PathClass::outside))) {
            verdict.kind = Verdict::Kind::deny;
            verdict.reason = verdict.approval.reason;
            return verdict;
        }
        if (intent.kind == tools::Intent::Kind::exec && !(sandbox_.landlock_abi > 0 && sandbox_.seccomp)) {
            log_agent()->warn("沙箱不可用，命令以 full_access 运行：{}", intent.summary);
            verdict.grant.sandbox = exec::Mode::full_access;
        }
        return answer(Verdict::Kind::allow);
    case PermissionMode::deny:
        verdict.kind = Verdict::Kind::deny;
        verdict.reason = "只读模式";
        return verdict;
    }
    return verdict;
}

tools::Grant Policy::grant_for_exec() const {
    tools::Grant grant;
    grant.sandbox = (sandbox_.landlock_abi > 0 && sandbox_.seccomp) ? exec::Mode::workspace_write
                                                                   : exec::Mode::full_access;
    grant.allow_network = false;
    return grant;
}

tools::Grant Policy::grant_for(const Approval& approval, const Decision& decision) const {
    tools::Grant grant = grant_for_exec();
    grant.allow_network = decision.network;
    if (approval.intent.kind != tools::Intent::Kind::exec) {
        grant.sandbox = exec::Mode::workspace_write;
        grant.allow_network = false;
    }
    return grant;
}

void Policy::remember(const Approval& approval, const Decision& decision) {
    if (decision.answer != Decision::Answer::allow_session) return;
    switch (approval.intent.kind) {
    case tools::Intent::Kind::write:
        session_edits_ = true;
        return;
    case tools::Intent::Kind::read:
        for (const auto& path : approval.intent.paths) {
            if (classify(path) == PathClass::outside) read_dirs_.push_back(path.path.parent_path());
        }
        return;
    case tools::Intent::Kind::external:
        external_rules_.push_back(approval.tool);
        return;
    case tools::Intent::Kind::exec: {
        const exec::Analysis analysis = exec::analyze(approval.intent.command);
        if (analysis.has_opaque) return;
        for (const auto& command : analysis.commands) {
            if (single_readonly(command)) continue;
            const auto prefix = command_prefix(command);
            if (!prefix) continue;
            const bool known = std::any_of(exec_rules_.begin(), exec_rules_.end(),
                                           [&](const ExecRule& rule) { return rule.prefix == *prefix; });
            if (!known) exec_rules_.push_back({*prefix, decision.network});
        }
        return;
    }
    }
}

void Policy::set_mode(PermissionMode mode) { mode_.store(mode); }

PermissionMode Policy::mode() const { return mode_.load(); }

bool parallel(const Verdict& verdict, const tools::Intent& intent) {
    if (verdict.kind != Verdict::Kind::allow) return false;
    if (intent.kind == tools::Intent::Kind::read) return true;
    return intent.kind == tools::Intent::Kind::exec && verdict.grant.sandbox == exec::Mode::read_only;
}

} // namespace dagent::agent
