#include "workspace/context.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <sstream>

#include <sys/utsname.h>

#include "base/log.hpp"
#include "base/text.hpp"
#include "exec/process.hpp"
#include "inja/inja.hpp"

namespace dagent::workspace {
namespace fs = std::filesystem;
namespace {

fs::path canonical_or_absolute(const fs::path& p) {
    std::error_code ec;
    fs::path result = fs::weakly_canonical(p, ec);
    if (!ec) return result;
    ec.clear();
    result = fs::absolute(p, ec);
    return ec ? p.lexically_normal() : result.lexically_normal();
}

std::string trim(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

struct GitOutput {
    bool ok = false;
    std::string text;
};

// 所有 git 调用都带 --no-optional-locks（不和用户同时在跑的 git 抢锁）和 core.quotepath=off（中文路径不转义）。
GitOutput run_git(const fs::path& cwd, std::vector<std::string> args, std::chrono::milliseconds timeout,
                  std::stop_token stop, const exec::Options& process) {
    exec::Command cmd;
    cmd.argv = {"git", "--no-optional-locks", "-c", "core.quotepath=off"};
    cmd.argv.insert(cmd.argv.end(), std::make_move_iterator(args.begin()), std::make_move_iterator(args.end()));
    cmd.cwd = cwd;
    cmd.timeout = timeout;
    try {
        const exec::Result result = exec::run(cmd, process, {}, stop);
        if (result.exit_code && *result.exit_code == 0) return {true, std::move(result.out)};
    } catch (const std::exception&) {
        // 没装 git、不是仓库、超时、被取消……都按「没有 git 信息」处理，不抛给调用方
    }
    return {};
}

GitStatus parse_git_status(const std::string& text) {
    GitStatus info;
    std::string oid;
    std::string head;
    std::size_t changed = 0;
    std::size_t untracked = 0;
    std::size_t unmerged = 0;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind("# branch.oid ", 0) == 0) {
            oid = line.substr(13);
        } else if (line.rfind("# branch.head ", 0) == 0) {
            head = line.substr(14);
        } else if (line.rfind("1 ", 0) == 0 || line.rfind("2 ", 0) == 0) {
            ++changed;
        } else if (line.rfind("u ", 0) == 0) {
            ++unmerged;
        } else if (line.rfind("? ", 0) == 0) {
            ++untracked;
        }
    }

    if (head == "(detached)") {
        info.branch = oid.substr(0, std::min<std::size_t>(7, oid.size()));
    } else {
        info.branch = head;
    }

    std::vector<std::string> parts;
    if (changed > 0) parts.push_back(std::format("{} modified files", changed));
    if (untracked > 0) parts.push_back(std::format("{} untracked files", untracked));
    if (unmerged > 0) parts.push_back(std::format("{} conflicts", unmerged));
    info.dirty = !parts.empty();
    info.status_summary = "working tree clean";
    if (!parts.empty()) {
        info.status_summary.clear();
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) info.status_summary += ", ";
            info.status_summary += parts[i];
        }
    }

    return info;
}

std::optional<GitInfo> collect_git(const fs::path& cwd, const ContextOptions& opt, std::stop_token stop) {
    // 三条命令互不依赖，并发跑，避免大仓库上慢慢叠加等待时间。
    auto root_future = std::async(std::launch::async, [&] {
        return run_git(cwd, {"rev-parse", "--show-toplevel"}, opt.git_timeout, stop, opt.process);
    });
    auto status_future = std::async(std::launch::async, [&] {
        return run_git(cwd, {"status", "--porcelain=v2", "--branch"}, opt.git_timeout, stop, opt.process);
    });
    auto log_future = std::async(std::launch::async, [&] {
        return run_git(cwd, {"log", "--oneline", "-n", "5"}, opt.git_timeout, stop, opt.process);
    });

    const GitOutput root = root_future.get();
    const GitOutput status = status_future.get();
    const GitOutput log = log_future.get();
    if (!root.ok || !status.ok) {
        base::logger("workspace")->debug("git unavailable: root_ok={} status_ok={}", root.ok, status.ok);
        return std::nullopt;
    }

    GitInfo info;
    info.root = canonical_or_absolute(trim(root.text));
    info.status = parse_git_status(status.text);

    std::istringstream commits(log.text);
    for (std::string line; std::getline(commits, line);) {
        if (!line.empty()) info.recent_commits.push_back(line);
    }
    return info;
}

// 由外到内收集，预算不够时先牺牲最外层的文件。
std::vector<Instructions> collect_instructions(const fs::path& cwd,
                                               const std::optional<fs::path>& repo_root,
                                               const ContextOptions& opt) {
    std::vector<fs::path> candidates;
    const fs::path top = repo_root ? *repo_root : cwd;
    std::vector<fs::path> dirs;
    for (fs::path dir = cwd;; dir = dir.parent_path()) {
        dirs.push_back(dir);
        if (dir == top || dir == dir.parent_path()) break; // 到达仓库根或文件系统根
    }
    std::reverse(dirs.begin(), dirs.end());
    for (const fs::path& dir : dirs) {
        for (const std::string& name : opt.instruction_files) candidates.push_back(dir / name);
    }

    struct Loaded {
        fs::path file;
        std::string content;
    };
    std::vector<Loaded> loaded;
    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec)) continue;
        std::ifstream in(candidate, std::ios::binary);
        if (!in) continue;
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad()) continue;
        loaded.push_back({candidate, std::move(content)});
    }

    std::size_t budget = opt.max_instructions_bytes;
    std::vector<Instructions> inner_first;
    for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {
        Instructions item;
        item.file = it->file;
        if (it->content.size() <= budget) {
            item.content = std::move(it->content);
            budget -= item.content.size();
        } else if (budget > 0) {
            item.content = base::truncate_middle(it->content, budget);
            item.truncated = true;
            budget = 0;
        } else {
            item.truncated = true; // 更外层的文件整体丢弃
            continue;
        }
        inner_first.push_back(std::move(item));
    }
    std::reverse(inner_first.begin(), inner_first.end());
    return inner_first;
}

} // namespace

std::optional<GitStatus> collect_git_status(const fs::path& cwd, const ContextOptions& opt,
                                           std::stop_token stop) {
    const auto status = run_git(cwd, {"status", "--porcelain=v2", "--branch"},
                                opt.git_timeout, stop, opt.process);
    if (!status.ok) return std::nullopt;
    return parse_git_status(status.text);
}

Environment collect_environment(const fs::path& cwd, const ContextOptions& opt, std::stop_token stop) {
    if (stop.stop_requested()) throw WorkspaceError(WorkspaceError::Kind::cancelled, "context collection cancelled");

    Environment env;
    env.cwd = canonical_or_absolute(cwd);

    struct utsname name {};
    env.os = ::uname(&name) == 0 ? std::format("{} {}", name.sysname, name.release) : "unknown";
    if (const char* shell = std::getenv("SHELL"); shell != nullptr && *shell != '\0') {
        env.shell = shell;
    } else {
        env.shell = "sh";
    }
    const std::time_t now = std::time(nullptr);
    std::tm local {};
    if (::localtime_r(&now, &local) != nullptr) {
        char date[16] = {};
        if (std::strftime(date, sizeof(date), "%Y-%m-%d", &local) > 0) env.date = date;
    }

    env.git = collect_git(env.cwd, opt, stop);
    env.instructions = collect_instructions(env.cwd, env.git ? std::optional(env.git->root) : std::nullopt, opt);
    return env;
}

nlohmann::json to_json(const Environment& env) {
    nlohmann::json json;
    json["cwd"] = env.cwd.string();
    json["os"] = env.os;
    json["shell"] = env.shell;
    json["date"] = env.date;
    if (env.git) {
        json["git"] = {
            {"root", env.git->root.string()},
            {"branch", env.git->status.branch},
            {"status_summary", env.git->status.status_summary},
            {"recent_commits", env.git->recent_commits},
        };
    } else {
        json["git"] = nullptr;
    }
    json["instructions"] = nlohmann::json::array();
    for (const Instructions& instruction : env.instructions) {
        json["instructions"].push_back({
            {"file", instruction.file.string()},
            {"content", instruction.content},
            {"truncated", instruction.truncated},
        });
    }
    return json;
}

std::string render(std::string_view tmpl, const nlohmann::json& data) {
    try {
        inja::Environment env;
        env.add_callback("indent", 2, [](inja::Arguments& args) -> nlohmann::json {
            const std::string text = args.at(0)->get<std::string>();
            const int width = std::max(args.at(1)->get<int>(), 0);
            const std::string pad(static_cast<std::size_t>(width), ' ');
            std::string out;
            for (std::size_t start = 0;;) {
                if (!out.empty()) out += '\n';
                const std::size_t newline = text.find('\n', start);
                const std::size_t end = newline == std::string::npos ? text.size() : newline;
                out += pad;
                out.append(text, start, end - start);
                if (newline == std::string::npos) break;
                start = newline + 1;
            }
            return out;
        });
        return env.render(std::string(tmpl), data);
    } catch (const inja::InjaError& e) {
        if (e.location.line > 0) {
            throw WorkspaceError(WorkspaceError::Kind::bad_template,
                                 std::format("line {}: {}", e.location.line, e.message));
        }
        throw WorkspaceError(WorkspaceError::Kind::bad_template, e.message);
    }
}

} // namespace dagent::workspace
