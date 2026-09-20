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

// 会话授权匹配时不需要规则的命令：单独看是已知只读的，以及 cd——它只改变这条命令里后续命令的
// 工作目录，后续命令仍要各自命中规则；提示词让模型用 `cd dir && …` 换目录，不忽略它规则就永远匹配不上。
bool single_readonly(const exec::SimpleCommand& command) {
    if (!command.argv.empty() && command.argv.front() == "cd") return true;
    exec::Analysis analysis;
    analysis.commands.push_back(command);
    return exec::is_known_readonly(analysis);
}

// 选「本会话允许」时会记住的前缀：每条不是已知只读的简单命令一个，去重、保持顺序。
// 含无法静态判断的结构时不提供会话授权，返回空（docs/design/agent.md §7）。
std::vector<std::string> session_prefixes(const std::string& command) {
    const exec::Analysis analysis = exec::analyze(command);
    if (analysis.has_opaque) return {};
    std::vector<std::string> prefixes;
    for (const auto& simple : analysis.commands) {
        if (single_readonly(simple)) continue;
        const auto prefix = command_prefix(simple);
        if (!prefix) return {};
        if (std::find(prefixes.begin(), prefixes.end(), *prefix) == prefixes.end()) prefixes.push_back(*prefix);
    }
    return prefixes;
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

std::optional<bool> Policy::matches_session(const Approval& approval, const tools::Intent& intent) const {
    switch (intent.kind) {
    case tools::Intent::Kind::write:
        if (!session_edits_) return std::nullopt;
        // 会话授权只覆盖工作区内的普通文件；受保护文件与工作区外永远要用户亲自确认。
        if (std::all_of(intent.paths.begin(), intent.paths.end(), [&](const workspace::Resolved& path) {
                return classify(path) == PathClass::normal;
            })) {
            return false;
        }
        return std::nullopt;
    case tools::Intent::Kind::read:
        for (const auto& path : intent.paths) {
            for (const auto& dir : read_dirs_) {
                if (is_relative_to(path.path, dir)) return false;
            }
        }
        return std::nullopt;
    case tools::Intent::Kind::external:
        if (std::find(external_rules_.begin(), external_rules_.end(), approval.tool) !=
            external_rules_.end()) {
            return false;
        }
        return std::nullopt;
    case tools::Intent::Kind::exec: {
        const exec::Analysis analysis = exec::analyze(intent.command);
        if (analysis.has_opaque) return std::nullopt;
        bool network = false;
        for (const auto& command : analysis.commands) {
            if (single_readonly(command)) continue;
            const auto prefix = command_prefix(command);
            if (!prefix) return std::nullopt;
            const auto rule = std::find_if(exec_rules_.begin(), exec_rules_.end(),
                                           [&](const ExecRule& r) { return r.prefix == *prefix; });
            if (rule == exec_rules_.end()) return std::nullopt;
            network = network || rule->network;
        }
        return network;
    }
    }
    return std::nullopt;
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
            const std::vector<std::string> prefixes = session_prefixes(intent.command);
            if (!prefixes.empty()) {
                std::string list;
                for (const auto& prefix : prefixes) list += (list.empty() ? "`" : "、`") + prefix + "`";
                verdict.approval.session_rule = std::format("以后 {} 不再询问", list);
            }
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

    // 步骤 3：本会话授权。放行的 exec 命令用 workspace_write 沙箱；当初授权时勾了联网的记住联网
    // （docs/design/agent.md §7）。
    if (const std::optional<bool> session = matches_session(verdict.approval, intent)) {
        if (intent.kind == tools::Intent::Kind::exec) {
            verdict.grant = grant_for_exec(); // 沙箱不可用时降级为 full_access
            verdict.grant.allow_network = *session;
        }
        return answer(Verdict::Kind::allow);
    }

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
    case tools::Intent::Kind::exec:
        // 和 session_rule 里展示给用户的前缀是同一份（session_prefixes）。
        for (const std::string& prefix : session_prefixes(approval.intent.command)) {
            const auto rule = std::find_if(exec_rules_.begin(), exec_rules_.end(),
                                           [&](const ExecRule& r) { return r.prefix == prefix; });
            if (rule == exec_rules_.end()) exec_rules_.push_back({prefix, decision.network});
            else rule->network = rule->network || decision.network;
        }
        return;
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
