#include "ipc/channel.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dagent::ipc {

Channel::Channel(Channel&& other) noexcept
    : fd_(other.fd_), buffer_(std::move(other.buffer_)), error_(std::move(other.error_)),
      eof_(other.eof_) {
    other.fd_ = -1;
}

Channel& Channel::operator=(Channel&& other) noexcept {
    if (this == &other) return *this;
    close();
    fd_ = other.fd_;
    buffer_ = std::move(other.buffer_);
    error_ = std::move(other.error_);
    eof_ = other.eof_;
    other.fd_ = -1;
    return *this;
}

Channel::~Channel() { close(); }

std::pair<Channel, Channel> Channel::create_pair() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return {};
    return {Channel(fds[0]), Channel(fds[1])};
}

void Channel::set_cloexec() {
    if (fd_ < 0) return;
    const int flags = ::fcntl(fd_, F_GETFD);
    if (flags >= 0) ::fcntl(fd_, F_SETFD, flags | FD_CLOEXEC);
}

bool Channel::send_line(std::string_view line) {
    if (fd_ < 0) {
        error_ = "channel is closed";
        return false;
    }
    std::string data(line);
    data += '\n';
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written =
            ::send(fd_, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        error_ = written == 0 ? "connection closed" : std::strerror(errno);
        return false;
    }
    return true;
}

std::optional<std::string> Channel::receive_line(std::size_t max_bytes) {
    for (;;) {
        if (const std::size_t newline = buffer_.find('\n'); newline != std::string::npos) {
            std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            return line;
        }
        if (buffer_.size() > max_bytes) {
            error_ = "line exceeds the maximum size";
            return std::nullopt;
        }
        if (fd_ < 0) {
            error_ = "channel is closed";
            return std::nullopt;
        }
        char chunk[8192];
        const ssize_t read = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (read > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(read));
            continue;
        }
        if (read == 0) {
            eof_ = true;
            return std::nullopt;
        }
        if (errno == EINTR) continue;
        error_ = std::strerror(errno);
        return std::nullopt;
    }
}

void Channel::shutdown() {
    if (fd_ < 0) return;
    ::shutdown(fd_, SHUT_RDWR);
}

void Channel::close() {
    if (fd_ < 0) return;
    ::close(fd_);
    fd_ = -1;
}

BackendProcess::~BackendProcess() { channel.close(); }

std::filesystem::path backend_binary_path() {
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) return {};
    const std::filesystem::path backend = self.parent_path() / "dagent-backend";
    if (!std::filesystem::exists(backend, error)) return {};
    return backend;
}

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
    parent_end.set_cloexec();
    child_end.set_cloexec();

    const std::string fd_text = std::to_string(child_end.fd());
    const std::string path = binary.string();
    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        return std::nullopt;
    }
    if (pid == 0) {
        // 子进程：独立进程组，只保留 stdin/stdout/stderr 与传递的 IPC FD。
        ::setpgid(0, 0);
        const int ipc_fd = child_end.fd();
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
    return BackendProcess(std::move(parent_end), pid);
}

bool wait_or_terminate(BackendProcess& process, std::chrono::milliseconds grace) {
    if (process.pid <= 0) return true;
    process.channel.shutdown();
    const auto deadline = std::chrono::steady_clock::now() + grace;
    for (;;) {
        int status = 0;
        const pid_t result = ::waitpid(process.pid, &status, WNOHANG);
        if (result == process.pid || (result < 0 && errno == ECHILD)) {
            process.pid = -1;
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(process.pid, SIGTERM);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int status = 0;
    if (::waitpid(process.pid, &status, WNOHANG) != process.pid) {
        ::kill(process.pid, SIGKILL);
        ::waitpid(process.pid, &status, 0);
        process.pid = -1;
        return false;
    }
    process.pid = -1;
    return true;
}

} // namespace dagent::ipc
