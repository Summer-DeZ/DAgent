#include "exec/detail.hpp"

#include "exec/sandbox.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <fcntl.h>
#include <fnmatch.h>

namespace dagent::exec::detail {
namespace {

bool is_denied(std::string_view name, const std::vector<std::string>& patterns) {
    for (const auto& pattern : patterns) {
        if (::fnmatch(pattern.c_str(), std::string(name).c_str(), FNM_CASEFOLD) == 0) return true;
    }
    return false;
}

bool is_executable(const std::filesystem::path& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    return ::access(path.c_str(), X_OK) == 0;
}

// 环境条目形如 "名字=值"；名字为空或没有 '=' 的条目直接忽略。
bool entry_has_name(const std::string& entry, std::string_view name) {
    return entry.size() > name.size() && entry[name.size()] == '=' &&
           std::string_view(entry).compare(0, name.size(), name) == 0;
}

void set_entry(std::vector<std::string>& env, std::string_view name, std::string_view value) {
    std::string entry = std::string(name) + "=" + std::string(value);
    for (auto& existing : env) {
        if (entry_has_name(existing, name)) {
            existing = std::move(entry);
            return;
        }
    }
    env.push_back(std::move(entry));
}

void remove_entry(std::vector<std::string>& env, std::string_view name) {
    std::erase_if(env, [&](const std::string& entry) { return entry_has_name(entry, name); });
}

// 默认注入的变量（坑 7）：防止命令进分页器、停下来等输入、吐颜色转义。
// 调用方可以通过 env_unset 撤掉其中任意一个，通过 env_set 覆盖成别的值。
constexpr std::pair<std::string_view, std::string_view> kDefaultEnv[] = {
    {"PAGER", "cat"}, {"GIT_PAGER", "cat"}, {"GIT_TERMINAL_PROMPT", "0"},
    {"TERM", "dumb"}, {"NO_COLOR", "1"},
};

} // namespace

std::vector<std::string> make_environment(const Command& cmd, const Options& opt) {
    std::vector<std::string> env;
    for (char** entry = ::environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view view(*entry);
        const auto equals = view.find('=');
        if (equals == std::string_view::npos || equals == 0) continue;
        if (is_denied(view.substr(0, equals), opt.env_deny)) continue;
        env.emplace_back(view);
    }

    for (const auto& [name, value] : kDefaultEnv) set_entry(env, name, value);
    for (const auto& name : cmd.env_unset) remove_entry(env, name);
    for (const auto& [name, value] : cmd.env_set) set_entry(env, name, value);
    return env;
}

std::string_view find_env(const std::vector<std::string>& env, std::string_view name) {
    for (const auto& entry : env) {
        if (entry_has_name(entry, name)) return std::string_view(entry).substr(name.size() + 1);
    }
    return {};
}

std::filesystem::path resolve_program(std::string_view name, const std::filesystem::path& cwd,
                                      std::string_view path_env) {
    if (name.empty()) return {};

    // 子进程会先 chdir 到 cwd 再 exec，所以带目录的相对路径要相对 cwd 检查，
    // 并解析成绝对路径传给 exec，检查的和执行的才是同一个文件。
    const std::filesystem::path base = cwd.empty() ? std::filesystem::current_path() : cwd;
    const std::filesystem::path path(name);
    if (path.has_parent_path()) {
        const auto candidate = path.is_absolute() ? path : base / path;
        return is_executable(candidate) ? candidate : std::filesystem::path{};
    }
    if (path_env.empty()) return {};

    while (true) {
        const auto colon = path_env.find(':');
        const std::string_view dir = path_env.substr(0, colon);
        const std::filesystem::path dir_path = dir.empty() ? base : std::filesystem::path(dir);
        const auto candidate = (dir_path.is_absolute() ? dir_path : base / dir_path) / path;
        if (is_executable(candidate)) return candidate;
        if (colon == std::string_view::npos) break;
        path_env.remove_prefix(colon + 1);
    }
    return {};
}

void ignore_sigpipe() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction current {};
        if (::sigaction(SIGPIPE, nullptr, &current) == 0 && current.sa_handler == SIG_DFL)
            ::signal(SIGPIPE, SIG_IGN);
    });
}

int setup_child(const Prepared* sandbox, const char* cwd) noexcept {
    if (::setsid() == -1) return errno;
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    if (::sigaction(SIGPIPE, &dfl, nullptr) == -1) return errno;
    // 信号屏蔽会经 fork、exec 原样继承：父进程为 sigwait 屏蔽的 SIGINT/SIGTERM 不能带进子进程，
    // 否则 `timeout`、SIGTERM 清理都会失效。
    sigset_t none;
    sigemptyset(&none);
    if (::sigprocmask(SIG_SETMASK, &none, nullptr) == -1) return errno;
    if (cwd != nullptr && ::chdir(cwd) == -1) return errno;
    if (sandbox != nullptr) return apply_in_child(*sandbox);
    return 0;
}

PipeFds::~PipeFds() {
    if (read >= 0) ::close(read);
    if (write >= 0) ::close(write);
}

void make_pipe(PipeFds& pipe) {
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) == -1)
        throw ExecError{ExecError::Kind::spawn_failed, std::string("pipe2: ") + std::strerror(errno)};
    pipe.read = fds[0];
    pipe.write = fds[1];
}

} // namespace dagent::exec::detail
