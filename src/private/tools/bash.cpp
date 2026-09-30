#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <utility>

#include "base/text.hpp"
#include "exec/shell.hpp"
#include "tools/detail.hpp"

namespace dagent::tools {
using detail::error_result;
using detail::require_string;

namespace {

constexpr std::string_view kDescription = R"(Run a bash command in the workspace root.

- The default environment is managed by DAgent. Use environment=project only for the user project's host toolchain; use a skill's configured environment when instructed.
- Every call already starts in the workspace root. Do not prefix commands with cd to that same directory; use relative paths. Use cd subdir && ... only to enter a different directory. Directory and environment changes do not persist between calls.
- stdout and stderr are returned together. Commands are terminated on timeout; timeout_ms is in milliseconds and capped at 10 minutes.
- Commands run in a sandbox by default: no writes outside the workspace (except /tmp) and no network access. Sandbox restrictions are reported in the result. Use another approach or explain the restriction to the user; do not keep retrying.
- Background daemons (such as server &) are not supported; they are cleaned up when the main process exits.
- Read-only commands (such as git status and ls) are allowed automatically without approval.)";


class BashCall final : public PreparedTool {
public:
    BashCall(const Context& ctx, std::string command,
             exec::Analysis analysis, std::optional<std::chrono::milliseconds> timeout,
             std::string environment_name, exec::Environment environment)
        : root_(ctx.root()), process_options_(ctx.process()),
          max_result_bytes_(ctx.options().max_result_bytes), collect_bytes_(ctx.options().bash_collect_bytes),
          srt_(ctx.options().srt), sandbox_state_root_(ctx.options().sandbox_state_root),
          sandbox_options_(ctx.options().sandbox), command_(std::move(command)),
          environment_name_(std::move(environment_name)), environment_(std::move(environment)),
          analysis_(std::move(analysis)), timeout_(timeout) {
        intent_.kind = agent::ToolKind::exec;
        agent::CommandIntent cmd;
        cmd.command = "[environment=" + environment_name_ + "] " + command_;
        cmd.analysis_version = analysis_.version;
        cmd.syntax = analysis_.syntax == exec::SyntaxStatus::valid   ? agent::SyntaxState::valid
                     : analysis_.syntax == exec::SyntaxStatus::error ? agent::SyntaxState::error
                                                                     : agent::SyntaxState::incomplete;
        cmd.syntax_message = analysis_.syntax_message;
        cmd.dynamic = analysis_.dynamic;
        cmd.cwd_unknown = analysis_.cwd_unknown;
        cmd.known_readonly = exec::is_known_readonly(analysis_, root_);
        cmd.dangerous = exec::is_dangerous(analysis_);
        cmd.impacts.reserve(analysis_.impacts.size());
        for (const exec::Impact& impact : analysis_.impacts) {
            const agent::ImpactKind kind = impact.kind == exec::ImpactKind::read   ? agent::ImpactKind::read
                                           : impact.kind == exec::ImpactKind::write ? agent::ImpactKind::write
                                           : impact.kind == exec::ImpactKind::network ? agent::ImpactKind::network
                                                                                      : agent::ImpactKind::special;
            cmd.impacts.push_back(agent::CommandImpact{kind, impact.target, impact.reason, impact.dynamic});
        }
        intent_.command = std::move(cmd);
        auto line = command_;
        if (const auto nl = line.find('\n'); nl != std::string::npos) line = line.substr(0, nl);
        if (line.size() > 100) line = line.substr(0, 100);
        intent_.summary = std::format("Run [{}] {}", environment_name_, line);
    }

private:
    Result do_execute(const Grant& grant, const std::function<void(std::string_view)>& on_output,
                      std::stop_token stop) override {
        const exec::Mode mode = grant.sandbox == agent::SandboxProfile::read_only    ? exec::Mode::read_only
                                : grant.sandbox == agent::SandboxProfile::full_access ? exec::Mode::full_access
                                                                                      : exec::Mode::workspace_write;
        const bool sandboxed = mode != exec::Mode::full_access;
        exec::Policy policy;
        if (sandboxed) {
            policy.mode = mode;
            policy.allow_network = grant.allow_network;
            policy.allow_local_sockets = grant.allow_local_sockets;
            policy.private_tmp = grant.private_tmp;
            policy.protect_sensitive_names = grant.protect_sensitive_names;
            policy.readable = grant.readable;
            policy.writable = grant.writable;
            policy.protected_read = grant.protected_read;
            policy.protected_write = grant.protected_write;
            policy.network_targets = grant.network_targets;
        }
        if (sandboxed && (!srt_.has_value() || grant.backend != "srt"))
            return error_result("restricted execution is unavailable; run `dagent sandbox status` and fix the sandbox runtime");

        std::string collected;
        collected.reserve(std::min(collect_bytes_, process_options_.max_output_bytes));
        const auto on_chunk = [&](exec::Stream stream, std::string_view chunk) {
            if (stream != exec::Stream::out) return;
            if (on_output) on_output(chunk);
            collected.append(chunk.substr(0, collect_bytes_ - collected.size()));
        };

        const auto started = std::chrono::steady_clock::now();
        std::optional<exec::Result> outcome;
        bool interrupted = false, spawn_failed = false, run_failed = false;
        std::string failure;
        try {
            if (sandboxed) {
                exec::SrtRequest request;
                request.runtime = *srt_;
                request.workspace = root_;
                request.state_root = sandbox_state_root_;
                request.command = command_;
                request.policy = policy;
                request.environment = environment_.variables;
                request.timeout = timeout_;
                request.startup_timeout = sandbox_options_.startup_timeout;
                request.approval_timeout = sandbox_options_.network_approval_timeout;
                request.max_network_requests = sandbox_options_.max_network_requests_per_execution;
                request.network_gate = [gate = grant.network_decider](std::string_view host, int port,
                                                                      std::string& reason, std::stop_token execution_stop) {
                    if (!gate) {
                        reason = "runtime network approval is unavailable in this run";
                        return exec::NetworkGateResult::deny;
                    }
                    switch (gate(agent::NetworkTarget{std::string(host), port}, reason, execution_stop)) {
                    case agent::NetworkAction::allow: return exec::NetworkGateResult::allow;
                    case agent::NetworkAction::cancel: return exec::NetworkGateResult::cancel;
                    case agent::NetworkAction::deny: break;
                    }
                    return exec::NetworkGateResult::deny;
                };
                if (environment_name_ == "project") {
                    const char* path = std::getenv("PATH");
                    request.environment.emplace_back(
                        "PATH", path && *path ? std::string(path)
                                              : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
                }
                // 固定消息语言：报错文本（strerror）不随系统 locale 变化，给模型和沙箱判断都是稳定输入。
                // 用 C.UTF-8 而不是 C，否则 ls 会把中文文件名转义成 \346… 这样的八进制。
                request.environment.emplace_back("LC_ALL", "C.UTF-8");
                outcome = exec::run_srt(request, process_options_, on_chunk, stop);
            } else {
                exec::Command cmd;
                cmd.argv = {environment_.shell.string(), "--noprofile", "--norc", "-c", command_};
                cmd.cwd = root_;
                cmd.merge_stderr = true;
                cmd.timeout = timeout_;
                cmd.inherit_env = environment_name_ == "project";
                cmd.env_set = environment_.variables;
                if (environment_name_ == "project") {
                    process_options_.environment.clear();
                    if (const char* path = std::getenv("PATH")) cmd.env_set.emplace_back("PATH", path);
                }
                cmd.env_set.emplace_back("LC_ALL", "C.UTF-8");
                outcome = exec::run(cmd, process_options_, on_chunk, stop);
            }
        } catch (const exec::ExecError& e) {
            if (e.kind() == exec::ExecError::Kind::cancelled) interrupted = true;
            else {
                spawn_failed = e.kind() == exec::ExecError::Kind::spawn_failed;
                run_failed = !spawn_failed; // 沙箱应用失败等执行层问题，命令没有跑完
                failure = e.what();
            }
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);

        agent::BashView view;
        view.command = "[" + environment_name_ + "] " + command_;
        view.sandbox = std::string(agent::to_string(grant.sandbox));
        view.backend = grant.backend;
        view.grant_source = std::string(agent::to_string(grant.source));
        view.analysis_version = grant.analysis_version;
        view.allow_network = grant.allow_network;
        view.allow_local_sockets = grant.allow_local_sockets;
        view.private_tmp = grant.private_tmp;
        view.protect_sensitive_names = grant.protect_sensitive_names;
        const auto copy_paths = [](const auto& paths) {
            std::vector<std::string> result;
            result.reserve(paths.size());
            for (const auto& path : paths) result.push_back(path.string());
            return result;
        };
        view.readable = copy_paths(grant.readable);
        view.writable = copy_paths(grant.writable);
        view.protected_read = copy_paths(grant.protected_read);
        view.protected_write = copy_paths(grant.protected_write);
        view.network_targets = grant.network_targets;
        view.elapsed_ms = elapsed.count();

        std::string raw; // 给视图的「完整输出」（exec 已按上限保留头尾）
        if (outcome) {
            raw = base::to_valid_utf8(base::strip_ansi(outcome->out));
            view.exit_code = outcome->exit_code;
            view.signal = outcome->signal;
            view.timed_out = outcome->timed_out;
        } else {
            raw = base::to_valid_utf8(base::strip_ansi(collected));
        }

        // 状态行放在截断之后：先按预算截正文，再追加说明，保证说明本身不会被切掉。
        // truncate_middle 的标记和状态行都不占正文预算：预留出这段长度，正文截得更短，
        // 追加之后整体仍在 max_result_bytes_ 内，最后不需要再整体 resize。
        constexpr std::size_t kStatusRoom = 512;
        const std::size_t body_budget =
            max_result_bytes_ > kStatusRoom ? max_result_bytes_ - kStatusRoom : max_result_bytes_ / 2;
        std::string body = base::truncate_middle(raw, body_budget);
        std::string text = body.empty() ? "(no output)" : body;

        view.output = raw;
        Result result;
        if (interrupted) {
            text += "\n[interrupted by the user]";
            view.interrupted = true;
        } else if (spawn_failed || run_failed) {
            text += std::format("\n[{}: {}]", spawn_failed ? "command could not be executed" : "command failed", failure);
            result.model_text = std::move(text);
            result.is_error = true;
            result.display = std::move(view);
            return result;
        } else if (outcome) {
            if (outcome->timed_out)
                text += std::format("\n[timed out after {}s]",
                                    std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
            if (outcome->signal)
                text += std::format("\n[terminated by signal {}]", *outcome->signal);
            else if (outcome->exit_code && *outcome->exit_code != 0)
                text += std::format("\n[exit code {}]", *outcome->exit_code);
        }
        result.model_text = std::move(text);
        result.is_error = !interrupted && outcome.has_value() &&
                          ((outcome->exit_code.has_value() && *outcome->exit_code != 0) ||
                           outcome->signal.has_value() || outcome->timed_out);
        result.interrupted = interrupted;
        result.display = std::move(view);
        return result;
    }

    std::filesystem::path root_;
    exec::Options process_options_;
    std::size_t max_result_bytes_ = 0;
    std::size_t collect_bytes_ = 0;
    std::optional<exec::SrtRuntime> srt_;
    std::filesystem::path sandbox_state_root_;
    exec::SandboxOptions sandbox_options_;
    std::string command_;
    std::string environment_name_;
    exec::Environment environment_;
    exec::Analysis analysis_; ///< 完整分析树只在实现里；核心只看 CommandIntent 摘要
    std::optional<std::chrono::milliseconds> timeout_;
};

class BashTool final : public Tool {
public:
    BashTool() {
        spec_.name = "bash";
        spec_.description = std::string(kDescription);
        spec_.parameters = {
            {"type", "object"},
            {"properties",
             {{"command", {{"type", "string"}, {"description", "Bash command to execute"}}},
              {"environment", {{"type", "string"}, {"description", "managed (default), project (host toolchain), or a configured skills/<name> environment"}}},
              {"timeout_ms",
               {{"type", "integer"}, {"description", "Timeout in milliseconds; maximum 600000 (10 minutes), default 300000"}}}}},
            {"required", std::vector<std::string>{"command"}},
        };
    }

    const Spec& spec() const override { return spec_; }

    std::expected<std::unique_ptr<PreparedTool>, Result> prepare(
        std::string_view arguments, Context& ctx) const override {
        auto args = detail::parse_arguments(arguments);
        if (!args) return std::unexpected(error_result(args.error()));
        std::string err;
        const std::string command = require_string(*args, "command", err);
        const auto timeout_ms = detail::get_int(*args, "timeout_ms", err);
        if (!err.empty()) return std::unexpected(error_result(err));

        std::string environment_name = detail::get_string(*args, "environment", err).value_or("managed");
        if (!err.empty()) return std::unexpected(error_result(err));
        exec::Environment environment;
        if (environment_name == "project") {
            environment.shell = ctx.options().environments.at("managed").shell;
        } else {
            const auto found = ctx.options().environments.find(environment_name);
            if (found == ctx.options().environments.end()) return std::unexpected(error_result("Unknown runtime environment: " + environment_name));
            environment = found->second;
        }
        exec::Analysis analysis = exec::analyze(command);
        if (analysis.syntax != exec::SyntaxStatus::valid) {
            std::string message = analysis.syntax_message.empty() ? "bash syntax could not be analyzed"
                                                                  : analysis.syntax_message;
            if (analysis.syntax_range) message += std::format(" at byte {}", analysis.syntax_range->begin);
            return std::unexpected(error_result(message + "; command not executed"));
        }

        std::optional<std::chrono::milliseconds> timeout;
        if (timeout_ms && *timeout_ms > 0) {
            timeout = std::chrono::milliseconds(*timeout_ms);
            if (*timeout > ctx.options().bash_max_timeout) timeout = ctx.options().bash_max_timeout;
        } else {
            timeout = ctx.process().default_timeout; // exec 里 0 表示不限
        }
        return std::make_unique<BashCall>(ctx, command, std::move(analysis), timeout, std::move(environment_name), std::move(environment));
    }

private:
    Spec spec_;
};

} // namespace

std::unique_ptr<Tool> detail::make_bash_tool() { return std::make_unique<BashTool>(); }

} // namespace dagent::tools
