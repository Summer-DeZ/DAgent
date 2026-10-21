#include "workspace/git.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "exec/process.hpp"
#include "base/path.hpp"
#include "workspace/diff.hpp"
#include "workspace/files.hpp"

namespace dagent::workspace {
namespace {

/// @brief 从文件所在目录向上找包含 .git（目录或文件）的仓库根；找不到返回空。
std::optional<std::filesystem::path> find_repository_root(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::path current = std::filesystem::absolute(file, ec).parent_path();
    if (ec || current.empty()) return std::nullopt;
    while (true) {
        if (std::filesystem::exists(current / ".git", ec)) return current;
        const std::filesystem::path parent = current.parent_path();
        if (parent == current) return std::nullopt;  // 已到文件系统根
        current = parent;
    }
}

/// @brief 用 argv 方式执行 git（不经过 shell），捕获标准输出与退出码。
struct GitOutcome {
    bool spawned = false;
    int exit_code = -1;
    std::string out;
};

GitOutcome run_git(const std::filesystem::path& root, std::vector<std::string> args,
                   std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (stop.stop_requested() || remaining <= std::chrono::milliseconds::zero()) return {};
    exec::Command command;
    command.argv = {"git", "-C", base::path_to_utf8(root)};
    for (std::string& arg : args) command.argv.push_back(std::move(arg));
    command.cwd = root;
    command.merge_stderr = true;
    command.inherit_env = false;
    if (const char* path = std::getenv("PATH")) command.env_set.emplace_back("PATH", path);
    command.timeout = remaining;

    GitOutcome outcome;
    try {
        exec::Options options;
        options.max_output_bytes = (8 << 20) + 1;
        const exec::Result result = exec::run(command, options, {}, stop);
        outcome.spawned = true;
        outcome.exit_code = result.out.size() >= options.max_output_bytes || result.timed_out
                                ? -1 : result.exit_code.value_or(-1);
        outcome.out = std::move(result.out);
    } catch (const exec::ExecError&) {
        outcome.spawned = false;
    }
    return outcome;
}

/// @brief diff 正文行数：跳过 `---`/`+++` 文件头与 `@@` hunk 头，含上下文行。
std::size_t diff_content_lines(std::string_view text) {
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

} // namespace

HeadDiff head_diff(const std::filesystem::path& file,
                   std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
    HeadDiff out;

    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(file, ec);
    if (ec) {
        out.error = "cannot resolve the file path";
        return out;
    }
    const std::optional<std::filesystem::path> root = find_repository_root(absolute);
    if (!root) {
        out.error = "not a git repository";
        return out;
    }

    const std::filesystem::path relative = absolute.lexically_relative(*root);
    if (relative.empty() || *relative.begin() == "..") {
        out.error = "the file is outside the git repository";
        return out;
    }

    // ls-files --error-unmatch：退出码 1 表示未被跟踪，其它非 0 是 git 层面的失败。
    const GitOutcome tracked = run_git(*root, {"ls-files", "--error-unmatch", "--", base::path_to_utf8(relative)},
                                       deadline, stop);
    if (!tracked.spawned) {
        out.error = "git is not available";
        return out;
    }
    if (tracked.exit_code != 0) {
        if (tracked.exit_code == 1) {
            out.tracked = false;
            return out;
        }
        out.error = "git ls-files failed";
        return out;
    }

    // 取 HEAD 全文；无 HEAD（新仓库）或取不到时不做伪空 diff。
    const GitOutcome head = run_git(*root, {"show", "HEAD:" + base::path_to_generic_utf8(relative)}, deadline, stop);
    if (!head.spawned) {
        out.error = "git is not available";
        return out;
    }
    if (head.exit_code != 0) {
        out.error = "the file cannot be read from HEAD";
        return out;
    }
    if (head.out.find('\0') != std::string::npos) {
        out.error = "binary file";
        return out;
    }

    TextFile current;
    try {
        current = read_text(absolute);
    } catch (const WorkspaceError& e) {
        out.error = e.kind() == WorkspaceError::Kind::not_text ? "binary file" : "the file cannot be read";
        return out;
    }
    if (current.truncated) {
        out.error = "the script exceeds the complete-read limit";
        return out;
    }

    out.tracked = true;
    const Unified diff = unified_diff(head.out, current.content, base::path_to_generic_utf8(relative));
    out.text = diff.text;
    out.content_lines = diff_content_lines(out.text);
    return out;
}

} // namespace dagent::workspace
