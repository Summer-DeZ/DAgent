#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

#include "base/text.hpp"
#include "base/path.hpp"
#include "base/platform.hpp"
#include "exec/shell.hpp"
#include "tools/detail.hpp"
#include "workspace/diff.hpp"
#include "workspace/git.hpp"

namespace dagent::tools {
using detail::error_result;
using detail::require_string;

namespace {

// 单文件脚本证据的正文上限（计划 5.2）：超过不截断，直接转后续审批。
constexpr std::size_t kScriptReviewLineLimit = 300;

// diff 正文行数：跳过 `---`/`+++` 文件头与 `@@` hunk 头，含上下文行。
std::size_t count_diff_lines(std::string_view text) {
    std::size_t lines = 0;
    std::size_t index = 0;  // 当前行序号，前两行固定是文件头
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        const std::string_view line = text.substr(start, end - start);
        if (index >= 2 && !line.starts_with("@@ ") && !line.empty()) ++lines;
        ++index;
        if (newline == std::string_view::npos) break;
        start = newline + 1;
    }
    return lines;
}

constexpr std::string_view kDescriptionHead = R"(Run a bash command in the workspace root.

- The default environment is managed by DAgent. Use environment=project only for the user project's host toolchain; use a skill's configured environment when instructed.
- Every call already starts in the workspace root. Do not prefix commands with cd to that same directory; use relative paths. Use cd subdir && ... only to enter a different directory. Directory and environment changes do not persist between calls.
- stdout and stderr are returned together. Commands are terminated on timeout; timeout_ms is in milliseconds and capped at 10 minutes.)";

constexpr std::string_view kDescriptionSandbox = R"(
- Commands run in a sandbox by default: no writes outside the workspace (except /tmp) and no network access. Sandbox restrictions are reported in the result. Use another approach or explain the restriction to the user; do not keep retrying.
- Background daemons (such as server &) are not supported; they are cleaned up when the main process exits.
- Read-only commands (such as git status and ls) are allowed automatically without approval.)";

constexpr std::string_view kDescriptionHost = R"(
- Commands run directly on the host without a sandbox: they can reach the network and any file your account can access. Unless the session is unrestricted, every command needs approval; a read-only command can be allowed for the rest of the session.
- Background daemons (such as server &) are not supported; they are cleaned up when the main process exits.)";



class BashCall final : public PreparedTool {
public:
    BashCall(const Context& ctx, std::string command,
             exec::Analysis analysis, std::optional<std::chrono::milliseconds> timeout,
             std::string environment_name, exec::Environment environment)
        : root_(ctx.root()), process_options_(ctx.process()), files_(ctx.files()),
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
        // 只存中立值：目标脚本与只读读取路径按工作区根解析成绝对路径。
        const auto to_absolute = [this](const std::string& raw) {
            return workspace::resolve(root_, exec::shell_path_to_native(raw).string()).path;
        };
        const bool changes_cwd = analysis_.cwd_unknown || std::ranges::any_of(analysis_.commands, [](const auto& c) {
            return !c.argv.empty() && c.argv.front() == "cd";
        });
        cmd.script_targets.reserve(analysis_.script_targets.size());
        cmd.has_unresolved_execution = analysis_.has_unresolved_execution;
        for (const exec::ScriptTarget& target : analysis_.script_targets) {
            if (changes_cwd && !exec::shell_path_to_native(target.path).is_absolute()) {
                cmd.has_unresolved_execution = true;
                continue; // 不拿工作区根下的同名文件替代实际执行目标。
            }
            cmd.script_targets.push_back(agent::ScriptTarget{to_absolute(target.path), target.command_index});
        }
        for (const std::string& raw : exec::readonly_read_paths(analysis_)) {
            const std::filesystem::path native = exec::shell_path_to_native(raw);
            if (changes_cwd && !native.is_absolute()) continue;
            const auto resolved = workspace::resolve(root_, native.string());
            std::error_code ec;
            const bool directory = std::filesystem::is_directory(resolved.path, ec);
            cmd.read_paths.push_back(resolved.path);
            intent_.paths.push_back({resolved.path, agent::Access::read, resolved.inside_workspace, directory});
        }
        intent_.command = std::move(cmd);
        auto line = command_;
        if (const auto nl = line.find('\n'); nl != std::string::npos) line = line.substr(0, nl);
        if (line.size() > 100) line = line.substr(0, base::utf8_floor(line, 100));
        intent_.summary = std::format("Run [{}] {}", environment_name_, line);
    }

private:
    Result do_execute(const Grant& grant, const std::function<void(std::string_view)>& on_output,
                      std::stop_token stop) override {
        const exec::Mode mode = grant.sandbox == agent::SandboxProfile::read_only    ? exec::Mode::read_only
                                : grant.sandbox == agent::SandboxProfile::full_access ? exec::Mode::full_access
                                                                                      : exec::Mode::workspace_write;
        const bool sandboxed = mode != exec::Mode::full_access;
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
                auto request = detail::sandbox_request(grant, *srt_, root_, sandbox_state_root_, sandbox_options_);
                request.command = command_;
                request.environment = environment_.variables;
                request.timeout = timeout_;
                request.merge_stderr = true;
                if (environment_name_ == "project") {
                    request.environment.emplace_back("PATH", exec::project_search_path());
                }
                // 固定消息语言：报错文本（strerror）不随系统 locale 变化，给模型和沙箱判断都是稳定输入。
                // 用 C.UTF-8 而不是 C，否则 ls 会把中文文件名转义成 \346… 这样的八进制。
                request.environment.emplace_back("LC_ALL", "C.UTF-8");
                outcome = exec::run_srt(request, process_options_, on_chunk, stop);
            } else {
                exec::Command cmd;
                cmd.argv = {base::path_to_utf8(environment_.shell), "--noprofile", "--norc", "-c", command_};
                cmd.cwd = root_;
                cmd.merge_stderr = true;
                cmd.timeout = timeout_;
                cmd.inherit_env = environment_name_ == "project";
                cmd.env_set = environment_.variables;
                if (environment_name_ == "project") {
                    process_options_.environment.clear();
                    if (const auto path = base::environment_variable("PATH")) cmd.env_set.emplace_back("PATH", *path);
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
            view.timed_out = outcome->timed_out;
            // 退出状态按 shell 的 $? 约定：顶层进程被信号终止记为 128+N。沙箱里 bwrap 对内部的信号终止
            // 也只能这样回报，两种后端由此一致；超时由状态行说明，不再附带被终止时的退出码。
            if (!outcome->timed_out)
                view.exit_code = outcome->signal ? std::optional(128 + *outcome->signal) : outcome->exit_code;
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
            else if (view.exit_code && *view.exit_code != 0)
                text += std::format("\n[exit code {}]", *view.exit_code);
        }
        result.model_text = std::move(text);
        result.is_error = !interrupted && outcome.has_value() &&
                          (outcome->timed_out || (view.exit_code.has_value() && *view.exit_code != 0));
        result.interrupted = interrupted;
        result.display = std::move(view);
        return result;
    }

    // 运行中复审用的单目标文件证据：只读文件系统与不可变成员，不修改对象状态。
    std::expected<agent::ScriptEvidence, agent::DecisionError> script_evidence(
        const agent::ScriptTarget& target, std::chrono::system_clock::time_point session_started,
        agent::ReviewDeadline deadline, std::stop_token stop) const override {
        agent::ScriptEvidence evidence;
        evidence.target = target.path;

        std::error_code ec;
        const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(target.path, ec);
        if (ec) {
            evidence.kind = agent::ScriptEvidence::Kind::unavailable;
            evidence.reason = "the script file cannot be read";
            return evidence;
        }

        // 会话开始时间是 system_clock；换算到文件时钟后与 mtime 用同一口径比较。
        const auto since_start = std::chrono::system_clock::now() - session_started;
        const std::filesystem::file_time_type session_file =
            std::filesystem::file_time_type::clock::now() -
            std::chrono::duration_cast<std::filesystem::file_time_type::duration>(since_start);
        if (mtime <= session_file) {
            evidence.kind = agent::ScriptEvidence::Kind::unchanged;
            return evidence;
        }

        const auto unreadable = [&evidence](std::string reason) {
            evidence.kind = agent::ScriptEvidence::Kind::unavailable;
            evidence.reason = std::move(reason);
        };
        const workspace::HeadDiff head = workspace::head_diff(target.path, deadline, stop);
        if (!head.error.empty()) {
            if (head.error != "not a git repository") {
                unreadable(head.error);
            } else {
                // 不在 Git 仓库：退回全文，按全文行数计。
                try {
                    const workspace::TextFile file = workspace::read_text(target.path, files_);
                    evidence.kind = agent::ScriptEvidence::Kind::full_text;
                    evidence.text = file.content;
                    evidence.content_lines = detail::count_lines(evidence.text);
                    if (file.truncated) unreadable("the script exceeds the complete-read limit");
                } catch (const workspace::WorkspaceError& e) {
                    unreadable(e.kind() == workspace::WorkspaceError::Kind::not_text
                                   ? "binary file"
                                   : "the script file cannot be read");
                }
            }
        } else if (!head.tracked) {
            // 未跟踪：按整体新增生成 diff。
            try {
                const workspace::TextFile file = workspace::read_text(target.path, files_);
                evidence.kind = agent::ScriptEvidence::Kind::diff;
                evidence.text = workspace::unified_diff({}, file.content, target.path.string()).text;
                evidence.content_lines = count_diff_lines(evidence.text);
                if (file.truncated) unreadable("the script exceeds the complete-read limit");
            } catch (const workspace::WorkspaceError& e) {
                unreadable(e.kind() == workspace::WorkspaceError::Kind::not_text
                               ? "binary file"
                               : "the script file cannot be read");
            }
        } else if (!head.text.empty()) {
            evidence.kind = agent::ScriptEvidence::Kind::diff;
            evidence.text = head.text;
            evidence.content_lines = head.content_lines;
        } else {
            evidence.kind = agent::ScriptEvidence::Kind::diff; // mtime 已修改；空 HEAD diff 仍属于复审。
        }

        if (evidence.content_lines > kScriptReviewLineLimit) {
            evidence.kind = agent::ScriptEvidence::Kind::too_large;
            evidence.text.clear();
            evidence.reason = "the script diff exceeds the 300-line review limit";
        }

        const bool expired = deadline <= std::chrono::steady_clock::now();
        if (expired || stop.stop_requested()) {
            evidence.kind = agent::ScriptEvidence::Kind::unavailable;
            evidence.text.clear();
            evidence.reason = expired ? "review deadline exceeded" : "review cancelled";
        }
        return evidence;
    }

    std::filesystem::path root_;
    exec::Options process_options_;
    workspace::FileOptions files_;
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
    explicit BashTool(bool sandbox_supported) {
        spec_.name = "bash";
        spec_.description = std::string(kDescriptionHead);
        spec_.description += sandbox_supported ? kDescriptionSandbox : kDescriptionHost;
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

std::unique_ptr<Tool> detail::make_bash_tool(bool sandbox_supported) {
    return std::make_unique<BashTool>(sandbox_supported);
}

} // namespace dagent::tools
