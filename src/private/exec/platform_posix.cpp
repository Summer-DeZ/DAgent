#include "exec/platform.hpp"
#include "exec/detail.hpp"

#include "base/log.hpp"
#include "base/platform.hpp"

#include <boost/process/v2.hpp>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <fcntl.h>
#include <fnmatch.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dagent::exec::detail {
namespace asio = boost::asio;
namespace bp = boost::process::v2;
namespace fs = std::filesystem;

struct InheritedResources {
    std::vector<int> fds;
};

std::span<const char* const> inherited_environment() noexcept {
    const auto begin = ::environ;
    if (begin == nullptr) return {};
    auto end = begin;
    while (*end != nullptr) ++end;
    return {begin, static_cast<std::size_t>(end - begin)};
}

bool environment_name_matches(std::string_view name, const std::string& pattern) {
    return ::fnmatch(pattern.c_str(), std::string(name).c_str(), FNM_CASEFOLD) == 0;
}

bool regular_executable(const std::filesystem::path& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    return base::can_execute(path);
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
struct ExecSetup {
    const char* cwd = nullptr;
    std::span<const int> inherit_fds;

    boost::system::error_code on_setup(bp::posix::default_launcher& launcher, const bp::filesystem::path&,
                                       const char* const*) const {
        launcher.fd_whitelist.insert(launcher.fd_whitelist.end(), inherit_fds.begin(), inherit_fds.end());
        return {};
    }

    boost::system::error_code on_exec_setup(bp::posix::default_launcher&, const bp::filesystem::path&,
                                          const char* const*) const {
        for (const int fd : inherit_fds)
            if (::fcntl(fd, F_SETFD, 0) == -1) return {errno, boost::system::system_category()};
        const int err = setup_child(cwd);
        if (err != 0) errno = err; // 失败时 launcher 把 errno（而不是返回的 ec）写回父进程
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
void ProcessGroup::capture(const bp::process& process) noexcept { impl_->pid = process.id(); }
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
        proc_ = bp::process(ctx_, program, args, env_setup, setup, stdio);
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
        proc_ = bp::process(ctx_, program_, args, env_setup, setup, stdio);
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

namespace {

std::string system_error(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

} // namespace

struct SrtControl::Impl {
    int control[2] = {-1, -1};
};

SrtControl::SrtControl() : impl_(std::make_unique<Impl>()) {
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, impl_->control) != 0)
        throw ExecError{ExecError::Kind::sandbox, system_error("socketpair")};
}

SrtControl::~SrtControl() { close(); }

void SrtControl::inherit_bridge_endpoint(Command& cmd) const {
    auto inherited = std::make_shared<InheritedResources>();
    inherited->fds = {impl_->control[1]};
    cmd.inherited_resources = std::move(inherited);
}

std::string SrtControl::bridge_argument() const {
    return "--control-fd=" + std::to_string(impl_->control[1]);
}

std::optional<std::string> SrtControl::send_initial(std::string_view init_line) {
    for (std::size_t offset = 0; offset < init_line.size();) {
        const ssize_t written = ::send(impl_->control[0], init_line.data() + offset,
                                       init_line.size() - offset, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return system_error("control init write");
        offset += static_cast<std::size_t>(written);
    }
    return std::nullopt;
}

void SrtControl::send_decision(std::string_view line) {
    const ssize_t written = ::send(impl_->control[0], line.data(), line.size(), MSG_NOSIGNAL);
    if (written < 0 && errno != EPIPE)
        base::logger("exec")->warn("srt control write failed: {}", std::strerror(errno));
}

void SrtControl::send_start(std::string_view start) {
    ::send(impl_->control[0], start.data(), start.size(), MSG_NOSIGNAL);
}

std::size_t SrtControl::read_chunk(std::span<char> chunk) {
    const ssize_t bytes = ::read(impl_->control[0], chunk.data(), chunk.size());
    return bytes > 0 ? static_cast<std::size_t>(bytes) : 0;
}

void SrtControl::shutdown_host() noexcept { ::shutdown(impl_->control[0], SHUT_RDWR); }

void SrtControl::close_bridge() noexcept {
    ::close(impl_->control[1]);
    impl_->control[1] = -1;
}

void SrtControl::close() noexcept {
    for (int& fd : impl_->control) {
        if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); fd = -1; }
    }
}

std::vector<fs::path> default_srt_readable_paths() {
    return {
        "/bin", "/sbin", "/usr", "/lib", "/lib64", "/etc", "/dev", "/proc", "/sys",
    };
}

} // namespace dagent::exec::detail
