#include <algorithm>
#include <chrono>
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

- Every call already starts in the workspace root. Do not prefix commands with cd to that same directory; use relative paths. Use cd subdir && ... only to enter a different directory. Directory and environment changes do not persist between calls.
- stdout and stderr are returned together. Commands are terminated on timeout; timeout_ms is in milliseconds and capped at 10 minutes.
- Commands run in a sandbox by default: no writes outside the workspace (except /tmp) and no network access. Sandbox restrictions are reported in the result. Use another approach or explain the restriction to the user; do not keep retrying.
- Background daemons (such as server &) are not supported; they are cleaned up when the main process exits.
- Read-only commands (such as git status and ls) are allowed automatically without approval.)";

constexpr std::size_t kCollectCap = 4 << 20; // 内部收集上限：状态行提示与中断输出够用

bool looks_like_sandbox_denial(std::string_view output) {
    constexpr std::string_view kMarkers[] = {
        "Permission denied", "Read-only file system", "Operation not permitted",
        "Could not resolve host",
    };
    return std::ranges::any_of(kMarkers, [&](std::string_view marker) {
        return output.find(marker) != std::string_view::npos;
    });
}

class BashCall final : public Call {
public:
    BashCall(const Context& ctx, std::string command, std::optional<std::chrono::milliseconds> timeout)
        : root_(ctx.root()), process_options_(ctx.process()), max_result_bytes_(ctx.options().max_result_bytes),
          command_(std::move(command)), timeout_(timeout) {
        const exec::Analysis analysis = exec::analyze(command_);
        intent_.kind = Intent::Kind::exec;
        intent_.command = command_;
        intent_.known_readonly = exec::is_known_readonly(analysis, root_);
        auto line = command_;
        if (const auto nl = line.find('\n'); nl != std::string::npos) line = line.substr(0, nl);
        if (line.size() > 100) line = line.substr(0, 100);
        intent_.summary = std::format("Run {}", line);
    }

private:
    Result do_run(const Grant& grant, const std::function<void(std::string_view)>& on_output,
                  std::stop_token stop) override {
        const bool sandboxed = grant.sandbox != exec::Mode::full_access;
        std::unique_ptr<exec::Prepared> prepared;
        if (sandboxed) {
            exec::Policy policy;
            policy.mode = grant.sandbox;
            policy.allow_network = grant.allow_network;
            policy.writable = {root_, "/tmp"};
            try {
                prepared = exec::prepare(policy);
            } catch (const exec::ExecError& e) {
                return error_result(std::format("sandbox setup failed; command not executed: {}", e.what()));
            }
        }

        exec::Command cmd;
        cmd.argv = {"bash", "-c", command_};
        cmd.cwd = root_;
        cmd.merge_stderr = true;
        cmd.timeout = timeout_;
        // 固定消息语言：报错文本（strerror）不随系统 locale 变化，给模型和沙箱判断都是稳定输入。
        // 用 C.UTF-8 而不是 C，否则 ls 会把中文文件名转义成 \346… 这样的八进制。
        cmd.env_set.emplace_back("LC_ALL", "C.UTF-8");
        cmd.sandbox = prepared.get();

        std::string collected;
        collected.reserve(1 << 16);
        const auto on_chunk = [&](exec::Stream stream, std::string_view chunk) {
            if (stream != exec::Stream::out) return;
            if (on_output) on_output(chunk);
            if (collected.size() < kCollectCap) collected.append(chunk);
        };

        const auto started = std::chrono::steady_clock::now();
        std::optional<exec::Result> outcome;
        bool interrupted = false, spawn_failed = false, run_failed = false;
        std::string failure;
        try {
            outcome = exec::run(cmd, process_options_, on_chunk, stop);
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

        BashView view;
        view.command = command_;
        view.sandbox = std::string(detail::sandbox_name(grant.sandbox));
        view.allow_network = grant.allow_network;
        view.elapsed_ms = elapsed.count();

        std::string raw; // 给视图的「完整输出」（exec 已按上限保留头尾）
        if (outcome) {
            raw = base::to_valid_utf8(base::strip_ansi(outcome->out.text));
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
        std::string body = base::truncate_middle(raw, body_budget).text;
        std::string text = body.empty() ? "(no output)" : body;

        // 沙箱提示按文档只在失败时给；退出码 0 时输出里偶然出现这些字样不代表被拦截
        const bool failed =
            interrupted ? false
                        : outcome.has_value()
                              ? (outcome->timed_out || outcome->signal.has_value() ||
                                 (outcome->exit_code.has_value() && *outcome->exit_code != 0))
                              : true;

        view.output = raw;
        Result result;
        if (interrupted) {
            text += "\n[interrupted by the user]";
            view.interrupted = true;
        } else if (spawn_failed || run_failed) {
            text += std::format("\n[{}: {}]", spawn_failed ? "command could not be executed" : "command failed", failure);
            result.text = std::move(text);
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
        if (sandboxed && failed && looks_like_sandbox_denial(collected))
            text += "\n[The command ran in a sandbox: no writes outside the workspace and no network access. Use another approach, or explain which restrictions the user needs to relax.]";

        result.text = std::move(text);
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
    std::string command_;
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
              {"timeout_ms",
               {{"type", "integer"}, {"description", "Timeout in milliseconds; maximum 600000 (10 minutes), default 300000"}}}}},
            {"required", std::vector<std::string>{"command"}},
        };
    }

    const Spec& spec() const override { return spec_; }

    std::expected<std::unique_ptr<Call>, Result> prepare(std::string_view arguments,
                                                         Context& ctx) const override {
        auto args = detail::parse_arguments(arguments);
        if (!args) return std::unexpected(error_result(args.error()));
        std::string err;
        const std::string command = require_string(*args, "command", err);
        const auto timeout_ms = detail::get_int(*args, "timeout_ms", err);
        if (!err.empty()) return std::unexpected(error_result(err));

        std::optional<std::chrono::milliseconds> timeout;
        if (timeout_ms && *timeout_ms > 0) {
            timeout = std::chrono::milliseconds(*timeout_ms);
            if (*timeout > ctx.options().bash_max_timeout) timeout = ctx.options().bash_max_timeout;
        } else {
            timeout = ctx.process().default_timeout; // exec 里 0 表示不限
        }
        return std::make_unique<BashCall>(ctx, command, timeout);
    }

private:
    Spec spec_;
};

} // namespace

std::unique_ptr<Tool> detail::make_bash_tool() { return std::make_unique<BashTool>(); }

} // namespace dagent::tools
