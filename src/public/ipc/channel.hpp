/// @file channel.hpp
/// @brief 私有连接的字节传输：AF_UNIX socketpair、UTF-8 JSON Lines 分帧与后端进程启动。
///
/// 只做字节层：组帧、短写、EOF 与子进程生命周期；不解析业务、不共享其他 FD。
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>

namespace dagent::ipc {

/// @brief 一个 socketpair 端点；send/receive 需由各自线程分别调用（不保证内部加锁）。
class Channel {
public:
    Channel() = default;
    explicit Channel(int fd) : fd_(fd) {}
    Channel(Channel&& other) noexcept;
    Channel& operator=(Channel&& other) noexcept;
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    /// @brief 建立一对已连接端点。
    static std::pair<Channel, Channel> create_pair();

    bool valid() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    /// @brief 设为 CLOEXEC：工具/MCP 子进程不能继承私有连接 FD。
    void set_cloexec();

    /// @brief 发送一行（自动补 '\n'）；处理短写与 EINTR，失败记录 error。
    bool send_line(std::string_view line);

    /// @brief 读取一行；EOF 返回 nullopt 且 eof() 为真；超长行返回 nullopt 并设 error。
    std::optional<std::string> receive_line(std::size_t max_bytes = 64u << 20);

    /// @brief 半关闭并关闭本端；幂等。
    void shutdown();
    void close();

    bool eof() const { return eof_; }
    const std::string& error() const { return error_; }

private:
    int fd_ = -1;
    std::string buffer_;
    std::string error_;
    bool eof_ = false;
};

/// @brief 本进程创建的后端进程；负责宽限终止与回收。
class BackendProcess {
public:
    BackendProcess() = default;
    BackendProcess(Channel channel, pid_t pid) : channel(std::move(channel)), pid(pid) {}
    BackendProcess(BackendProcess&&) noexcept = default;
    BackendProcess& operator=(BackendProcess&&) noexcept = default;
    ~BackendProcess();
    BackendProcess(const BackendProcess&) = delete;
    BackendProcess& operator=(const BackendProcess&) = delete;

    Channel channel;
    pid_t pid = -1;
};

/// @brief 同安装目录下的 dagent-backend；找不到返回 nullopt。
std::filesystem::path backend_binary_path();

/// @brief 创建 socketpair 并直接启动后端（同目录、独立进程组、只继承 IPC FD）。
/// 失败返回 nullopt 并写 error。
std::optional<BackendProcess> spawn_backend(std::string& error);

/// @brief 宽限期内等待退出，超时发 SIGTERM/SIGKILL；返回是否已回收。
bool wait_or_terminate(BackendProcess& process, std::chrono::milliseconds grace);

} // namespace dagent::ipc
