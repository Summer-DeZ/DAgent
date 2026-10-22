#include "exec/platform.hpp"
#include "exec/detail.hpp"

#include "base/log.hpp"
#include "base/platform.hpp"

#include <boost/process/v2/posix/default_launcher.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>

#include <dlfcn.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dagent::exec {

std::filesystem::path system_git_program() { return "/usr/bin/git"; }

std::filesystem::path shell_path_to_native(std::string_view shell_path) {
    return std::filesystem::path(shell_path);
}

std::string default_search_path() { return "/usr/bin:/bin"; }

std::string project_search_path() {
    const char* path = std::getenv("PATH");
    return path && *path ? std::string(path)
                        : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
}

} // namespace dagent::exec

namespace dagent::exec::detail {
namespace asio = boost::asio;
namespace bp = boost::process::v2;
namespace fs = std::filesystem;

struct InheritedResources {
    std::vector<int> fds;
};

std::vector<std::string> inherited_environment() {
    std::vector<std::string> environment;
    if (::environ == nullptr) return environment;
    for (auto entry = ::environ; *entry != nullptr; ++entry) environment.emplace_back(*entry);
    return environment;
}

bool environment_names_equal(std::string_view left, std::string_view right) {
    return left == right;
}

bool environment_name_matches(std::string_view name, const std::string& pattern) {
    return ::fnmatch(pattern.c_str(), std::string(name).c_str(), FNM_CASEFOLD) == 0;
}

bool regular_executable(const std::filesystem::path& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    return base::can_execute(path);
}

std::filesystem::path resolve_program(std::string_view name, const std::filesystem::path& cwd,
                                      const std::vector<std::string>& environment) {
    if (name.empty()) return {};

    // 子进程会先 chdir 到 cwd 再 exec，所以带目录的相对路径要相对 cwd 检查，
    // 并解析成绝对路径传给 exec，检查的和执行的才是同一个文件。
    const std::filesystem::path base = cwd.empty() ? std::filesystem::current_path() : cwd;
    const std::filesystem::path path(name);
    if (path.has_parent_path()) {
        const auto candidate = path.is_absolute() ? path : base / path;
        return regular_executable(candidate) ? candidate : std::filesystem::path{};
    }
    auto path_env = find_env(environment, "PATH");
    if (path_env.empty()) return {};

    while (true) {
        const auto colon = path_env.find(':');
        const std::string_view dir = path_env.substr(0, colon);
        const std::filesystem::path dir_path = dir.empty() ? base : std::filesystem::path(dir);
        const auto candidate = (dir_path.is_absolute() ? dir_path : base / dir_path) / path;
        if (regular_executable(candidate)) return candidate;
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

namespace {

int setup_child(const char* cwd) noexcept {
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
    return 0;
}

// 转移所有权后把对应成员置 -1，析构只关闭仍持有的端点。
struct PipeFds {
    int read = -1;
    int write = -1;

    PipeFds() = default;
    PipeFds(const PipeFds&) = delete;
    PipeFds& operator=(const PipeFds&) = delete;
    ~PipeFds();
};

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

std::span<const int> inherited_fds(const Command& cmd) noexcept {
    return cmd.inherited_resources ? std::span<const int>(cmd.inherited_resources->fds) : std::span<const int>{};
}

void close_fds(int first, int last) noexcept {
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, static_cast<unsigned>(first), static_cast<unsigned>(last), 0u) == 0) return;
#endif
    for (int fd = first; fd <= last && fd < 1024; ++fd) ::close(fd);
}

// 关闭白名单（stdio、launcher 的错误回报管道、继承清单）之外的全部描述符。Boost 1.83 的 close_all_fds
// 在比较相邻白名单项时取的是同一项，白名单之间的描述符从不关闭，日志文件和 curl 的 socket 会随 exec
// 进入子进程。fork 之后只排序，不分配内存。
void close_except(std::vector<int>& keep) noexcept {
    std::sort(keep.begin(), keep.end());
    int next = 0;
    for (const int fd : keep) {
        if (fd > next) close_fds(next, fd - 1);
        if (fd >= next) next = fd + 1;
    }
    close_fds(next, std::numeric_limits<int>::max());
}

/// 默认 launcher 的 env 成员默认指向进程环境；这个 init 把它换成过滤后的环境。
/// 不用 Boost.Process 的 process_environment：1.83 的 header-only 模式下它没有提供
/// on_setup 的定义（需要单独编译的 libboost_process）。
struct EnvSetup {
    std::vector<const char*> entries;

    explicit EnvSetup(const std::vector<std::string>& env) {
        entries.reserve(env.size() + 1);
        for (const auto& entry : env) entries.push_back(entry.c_str());
        entries.push_back(nullptr);  // execve 要求以 nullptr 结尾
    }

    boost::system::error_code on_setup(bp::posix::default_launcher& launcher, const bp::filesystem::path&,
                                       const char* const*) const {
        launcher.env = entries.data();
        return {};
    }
};

/// fork 之后、exec 之前执行的钩子，具体步骤见 setup_child（只做 async-signal-safe 调用）。
/// 必须排在 stdio 绑定之后：关闭多余描述符要等 stdio 的 dup2 完成。
struct ExecSetup {
    const char* cwd = nullptr;
    std::span<const int> inherit_fds;

    boost::system::error_code on_setup(bp::posix::default_launcher& launcher, const bp::filesystem::path&,
                                       const char* const*) const {
        launcher.fd_whitelist.insert(launcher.fd_whitelist.end(), inherit_fds.begin(), inherit_fds.end());
        return {};
    }

    boost::system::error_code on_exec_setup(bp::posix::default_launcher& launcher, const bp::filesystem::path&,
                                          const char* const*) const {
        for (const int fd : inherit_fds)
            if (::fcntl(fd, F_SETFD, 0) == -1) return {errno, boost::system::system_category()};
        const int err = setup_child(cwd);
        if (err != 0) errno = err; // 失败时 launcher 把 errno（而不是返回的 ec）写回父进程
        if (err == 0) close_except(launcher.fd_whitelist);
        return {err, boost::system::system_category()};
    }

    /// exec 前失败（chdir、沙箱、execve）时 launcher 在子进程里调用 ::exit，它会执行父进程的
    /// atexit 与静态析构，并把 fork 时复制来的 stdio 缓冲再刷一遍（父进程没 flush 的输出重复出现）。
    /// 这里抢先 _exit；错误码此时已经写回父进程。Boost 1.83 探测这个钩子时用的参数表
    /// (launcher, ec, exe, argv, ec) 和实际调用 (launcher, exe, argv, ec) 不一致，只能写成变参。
    template <class... Args>
    [[noreturn]] void on_exec_error(Args&&...) const noexcept {
        ::_exit(127);
    }
};

} // namespace

struct ProcessGroup::Impl {
    int pid = -1;
};

ProcessGroup::ProcessGroup() : impl_(std::make_unique<Impl>()) {}
ProcessGroup::~ProcessGroup() = default;
void ProcessGroup::capture(const bp::process& process) { impl_->pid = process.id(); }
bool ProcessGroup::valid() const noexcept { return impl_->pid > 0; }
void ProcessGroup::terminate() noexcept { ::kill(-impl_->pid, SIGTERM); }
void ProcessGroup::force_kill() noexcept { ::kill(-impl_->pid, SIGKILL); }

ExitStatus process_exit_status(const bp::process& process) noexcept {
    const int raw_status = process.native_exit_code();
    ExitStatus result;
    if (WIFEXITED(raw_status)) result.code = WEXITSTATUS(raw_status);
    else if (WIFSIGNALED(raw_status)) result.signal = WTERMSIG(raw_status);
    return result;
}

void spawn_command(const Command& cmd_, const fs::path& program, const std::vector<std::string>& env,
                   asio::io_context& ctx_, bp::process& proc_, asio::writable_pipe& in_pipe_,
                   asio::readable_pipe& out_pipe_, asio::readable_pipe& err_pipe_,
                   ProcessGroup& group, bool& spawned_) {
    const EnvSetup env_setup(env);

    PipeFds in, out, err, null_in;
    if (cmd_.stdin_data) {
        make_pipe(in);
    } else {
        // 不给 stdin 数据时接 /dev/null，绝不继承调用方的 stdin。
        null_in.read = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (null_in.read == -1)
            throw ExecError{ExecError::Kind::spawn_failed,
                            std::string("open /dev/null: ") + std::strerror(errno)};
    }
    make_pipe(out);
    // merge 模式下 stderr 直接绑到 out.write（见下方 stdio），err 不持有任何 fd，
    // 否则两个 PipeFds 析构时会对同一个 fd 关两次，第二次可能关掉别的线程刚打开的 fd。
    if (!cmd_.merge_stderr) make_pipe(err);

    bp::process_stdio stdio{
        .in = bp::detail::process_input_binding{cmd_.stdin_data ? in.read : null_in.read},
        .out = bp::detail::process_output_binding{out.write},
        .err = bp::detail::process_error_binding{cmd_.merge_stderr ? out.write : err.write},
    };

    std::vector<std::string> args(cmd_.argv.begin() + 1, cmd_.argv.end());
    ExecSetup setup{cmd_.cwd.empty() ? nullptr : cmd_.cwd.c_str(), inherited_fds(cmd_)};

    try {
        proc_ = bp::process(ctx_, program, args, env_setup, stdio, setup);
    } catch (const boost::system::system_error& e) {
        throw ExecError{ExecError::Kind::spawn_failed, e.what()};
    }
    spawned_ = true;
    group.capture(proc_);

    // 父进程不再需要子进程那一端。
    if (null_in.read >= 0) {
        ::close(null_in.read);
        null_in.read = -1;
    }
    if (in.read >= 0) {
        ::close(in.read);
        in.read = -1;
    }
    ::close(out.write);
    out.write = -1;
    if (!cmd_.merge_stderr) {
        ::close(err.write);
        err.write = -1;
    }

    out_pipe_.assign(out.read);
    out.read = -1;
    if (!cmd_.merge_stderr) {
        err_pipe_.assign(err.read);
        err.read = -1;
    }
    if (cmd_.stdin_data) {
        in_pipe_.assign(in.write);
        in.write = -1;
    }
}

void spawn_child(const Command& cmd, const fs::path& program_, const std::vector<std::string>& env,
                 asio::io_context& ctx_, bp::process& proc_, asio::writable_pipe& in_pipe_,
                 asio::readable_pipe& out_pipe_, asio::readable_pipe& err_pipe_, ProcessGroup& group) {
    const EnvSetup env_setup(env);
    detail::ignore_sigpipe();  // MCP server 挂掉后再 write 应得到 EPIPE，而不是杀掉 agent

    PipeFds in, out, err;
    make_pipe(in);
    make_pipe(out);
    make_pipe(err);

    std::vector<std::string> args(cmd.argv.begin() + 1, cmd.argv.end());
    ExecSetup setup{cmd.cwd.empty() ? nullptr : cmd.cwd.c_str(), inherited_fds(cmd)};
    bp::process_stdio stdio{
        .in = bp::detail::process_input_binding{in.read},
        .out = bp::detail::process_output_binding{out.write},
        .err = bp::detail::process_error_binding{err.write},
    };

    try {
        proc_ = bp::process(ctx_, program_, args, env_setup, stdio, setup);
    } catch (const boost::system::system_error& e) {
        throw ExecError{ExecError::Kind::spawn_failed, e.what()};
    }
    group.capture(proc_);

    ::close(in.read);
    in.read = -1;
    ::close(out.write);
    out.write = -1;
    ::close(err.write);
    err.write = -1;
    in_pipe_.assign(in.write);
    in.write = -1;
    out_pipe_.assign(out.read);
    out.read = -1;
    err_pipe_.assign(err.read);
    err.read = -1;
}

} // namespace dagent::exec::detail

namespace dagent::exec::detail {

struct SharedLibrary::Impl {
    void* handle = nullptr;
    ~Impl() {
        if (handle != nullptr) ::dlclose(handle);
    }
};

SharedLibrary::SharedLibrary() : impl_(std::make_unique<Impl>()) {}
SharedLibrary::SharedLibrary(SharedLibrary&&) noexcept = default;
SharedLibrary& SharedLibrary::operator=(SharedLibrary&&) noexcept = default;
SharedLibrary::~SharedLibrary() = default;

SharedLibrary SharedLibrary::open(const std::filesystem::path& file) {
    SharedLibrary library;
    library.impl_->handle = ::dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library.impl_->handle == nullptr) {
        const char* error = ::dlerror();
        throw ExecError{ExecError::Kind::sandbox,
                        "load " + file.string() + ": " + (error != nullptr ? error : "dlopen failed")};
    }
    return library;
}

void* SharedLibrary::symbol(const char* name) const noexcept { return ::dlsym(impl_->handle, name); }

std::string sandbox_command_line(const std::filesystem::path& shell, std::string_view script) {
    return "exec " + shell_quote(shell.string()) + " --noprofile --norc -c " + shell_quote(script);
}

// /proc/net/tcp 每行：sl local_address rem_address st tx:rx tr:when retrnsmt uid ...；地址为
// 小端十六进制 IPv4 加大端十六进制端口。客户端一侧的条目是 本端=对端端口、远端=代理端口。
bool loopback_peer_is_current_user(unsigned short local_port, unsigned short peer_port) {
    const std::string local = std::format("0100007F:{:04X}", peer_port);
    const std::string remote = std::format("0100007F:{:04X}", local_port);
    std::ifstream table("/proc/net/tcp");
    std::string line;
    std::getline(table, line); // 表头
    while (std::getline(table, line)) {
        std::istringstream fields(line);
        std::string slot, local_address, remote_address, state, queues, timer, retransmits;
        unsigned long uid = 0;
        if (!(fields >> slot >> local_address >> remote_address >> state >> queues >> timer >> retransmits >> uid))
            continue;
        if (local_address == local && remote_address == remote) return uid == ::getuid();
    }
    return false;
}

} // namespace dagent::exec::detail
