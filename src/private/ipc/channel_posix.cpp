#include "ipc/channel.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dagent::ipc {

struct detail::EndpointState {
    int fd = -1;
};

InheritedEndpoint::InheritedEndpoint() noexcept = default;
InheritedEndpoint::InheritedEndpoint(InheritedEndpoint&&) noexcept = default;
InheritedEndpoint& InheritedEndpoint::operator=(InheritedEndpoint&&) noexcept = default;
InheritedEndpoint::~InheritedEndpoint() = default;

InheritedEndpoint InheritedEndpoint::parse_argument(const char* text) {
    InheritedEndpoint endpoint;
    endpoint.state_ = std::make_unique<detail::EndpointState>();
    endpoint.state_->fd = std::atoi(text);
    return endpoint;
}

bool InheritedEndpoint::valid() const noexcept { return state_ && state_->fd >= 0; }

Channel::Channel() noexcept = default;

Channel::Channel(Channel&& other) noexcept
    : native_(std::move(other.native_)), buffer_(std::move(other.buffer_)), error_(std::move(other.error_)),
      eof_(other.eof_) {}

Channel& Channel::operator=(Channel&& other) noexcept {
    if (this == &other) return *this;
    close();
    native_ = std::move(other.native_);
    buffer_ = std::move(other.buffer_);
    error_ = std::move(other.error_);
    eof_ = other.eof_;
    return *this;
}

Channel::~Channel() { close(); }

Channel Channel::adopt(InheritedEndpoint endpoint) {
    Channel channel;
    channel.native_ = std::move(endpoint.state_);
    return channel;
}

std::pair<Channel, Channel> Channel::create_pair() {
    Channel parent, child;
    parent.native_ = std::make_unique<detail::EndpointState>();
    child.native_ = std::make_unique<detail::EndpointState>();
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return {};
    parent.native_->fd = fds[0];
    child.native_->fd = fds[1];
    return {std::move(parent), std::move(child)};
}

bool Channel::valid() const noexcept { return native_ && native_->fd >= 0; }

void Channel::prevent_inheritance() {
    if (!valid()) return;
    const int flags = ::fcntl(native_->fd, F_GETFD);
    if (flags >= 0) ::fcntl(native_->fd, F_SETFD, flags | FD_CLOEXEC);
}

Channel::Transfer Channel::send_bytes(std::string_view bytes) {
    const ssize_t written = ::send(native_->fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (written > 0) return {Transfer::Kind::bytes, static_cast<std::size_t>(written), {}};
    if (written < 0 && errno == EINTR) return {Transfer::Kind::interrupted, 0, {}};
    if (written == 0) return {Transfer::Kind::closed, 0, {}};
    return {Transfer::Kind::failed, 0, std::strerror(errno)};
}

Channel::Transfer Channel::receive_bytes(std::span<char> bytes) {
    const ssize_t read = ::recv(native_->fd, bytes.data(), bytes.size(), 0);
    if (read > 0) return {Transfer::Kind::bytes, static_cast<std::size_t>(read), {}};
    if (read == 0) return {Transfer::Kind::closed, 0, {}};
    if (errno == EINTR) return {Transfer::Kind::interrupted, 0, {}};
    return {Transfer::Kind::failed, 0, std::strerror(errno)};
}

void Channel::shutdown() {
    if (!valid()) return;
    ::shutdown(native_->fd, SHUT_RDWR);
}

void Channel::close() {
    if (!valid()) return;
    ::close(native_->fd);
    native_->fd = -1;
}

struct BackendProcess::State {
    pid_t pid = -1;
};

BackendProcess::BackendProcess() noexcept = default;

BackendProcess::BackendProcess(Channel channel, std::shared_ptr<const State> state)
    : channel(std::move(channel)), state_(std::move(state)) {}

// 原默认移动复制 PID，源对象仍保留进程标识；回收状态不能随共享标识一起修改。
BackendProcess::BackendProcess(BackendProcess&& other) noexcept
    : channel(std::move(other.channel)), state_(other.state_), reaped_(other.reaped_) {}

BackendProcess& BackendProcess::operator=(BackendProcess&& other) noexcept {
    channel = std::move(other.channel);
    state_ = other.state_;
    reaped_ = other.reaped_;
    return *this;
}

BackendProcess::~BackendProcess() { channel.close(); }

namespace {

void close_from(int first) {
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, static_cast<unsigned>(first), ~0u, 0u) == 0) return;
#endif
    for (int fd = first; fd < 1024; ++fd) ::close(fd);
}

} // namespace

std::optional<BackendProcess> spawn_backend(std::string& error) {
    const std::filesystem::path binary = backend_binary_path();
    if (binary.empty()) {
        error = "dagent-backend was not found next to the frontend binary";
        return std::nullopt;
    }
    auto [parent_end, child_end] = Channel::create_pair();
    if (!parent_end.valid()) {
        error = "socketpair failed";
        return std::nullopt;
    }
    parent_end.prevent_inheritance();
    child_end.prevent_inheritance();

    const std::string fd_text = std::to_string(child_end.native_->fd);
    const std::string path = binary.string();
    auto state = std::make_shared<BackendProcess::State>();
    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        return std::nullopt;
    }
    if (pid == 0) {
        // 子进程：独立进程组，只保留 stdin/stdout/stderr 与传递的 IPC FD。
        ::setpgid(0, 0);
        const int ipc_fd = child_end.native_->fd;
        const int kept = 3;
        // 必须先关父端再 dup2：父端可能正好占用 kept，dup2 后那个 fd 已属于子端。
        parent_end.close();
        if (ipc_fd != kept) {
            if (::dup2(ipc_fd, kept) < 0) ::_exit(127);
            child_end.close();
        }
        const int flags = ::fcntl(kept, F_GETFD); // dup2 会清 CLOEXEC；同号时手动清
        if (flags >= 0) ::fcntl(kept, F_SETFD, flags & ~FD_CLOEXEC);
        close_from(kept + 1);
        const std::string kept_text = std::to_string(kept);
        const char* argv[] = {path.c_str(), "--ipc-fd", kept_text.c_str(), nullptr};
        ::execv(path.c_str(), const_cast<char* const*>(argv));
        ::_exit(127);
    }
    child_end.close();
    state->pid = pid;
    return BackendProcess(std::move(parent_end), std::move(state));
}

bool wait_or_terminate(BackendProcess& process, std::chrono::milliseconds grace) {
    const auto pid = process.reaped_ || !process.state_ ? -1 : process.state_->pid;
    if (pid <= 0) return true;
    process.channel.shutdown();
    const auto deadline = std::chrono::steady_clock::now() + grace;
    for (;;) {
        int status = 0;
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) {
            process.reaped_ = true;
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGTERM);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) != pid) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        process.reaped_ = true;
        return false;
    }
    process.reaped_ = true;
    return true;
}

} // namespace dagent::ipc
