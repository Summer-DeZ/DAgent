/// @file channel.hpp
/// @brief 私有连接的字节传输：UTF-8 JSON Lines 分帧与后端进程启动。
///
/// 只做字节层：组帧、短写、EOF 与子进程生命周期；不解析业务、不共享其他 FD。
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace dagent::ipc {

namespace detail {
struct EndpointState;
} // namespace detail

class BackendProcess;

/// @brief 继承端点的参数值；解析和销毁均不接管或关闭连接。
class InheritedEndpoint {
public:
    InheritedEndpoint() noexcept;
    InheritedEndpoint(InheritedEndpoint&&) noexcept;
    InheritedEndpoint& operator=(InheritedEndpoint&&) noexcept;
    ~InheritedEndpoint();
    InheritedEndpoint(const InheritedEndpoint&) = delete;
    InheritedEndpoint& operator=(const InheritedEndpoint&) = delete;

    static InheritedEndpoint parse_argument(const char* text);
    bool valid() const noexcept;

private:
    std::unique_ptr<detail::EndpointState> state_;
    friend class Channel;
};

/// @brief 一个连接端点；send/receive 需由各自线程分别调用（不保证内部加锁）。
class Channel {
public:
    Channel() noexcept;
    Channel(Channel&& other) noexcept;
    Channel& operator=(Channel&& other) noexcept;
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    /// @brief 建立一对已连接端点。
    static std::pair<Channel, Channel> create_pair();
    static Channel adopt(InheritedEndpoint endpoint);

    bool valid() const noexcept;

    /// @brief 禁止随后启动的工具/MCP 子进程继承私有连接。
    void prevent_inheritance();

    /// @brief 发送一行（自动补 '\n'）；处理短写与 EINTR，失败记录 error。
    bool send_line(std::string_view line);

    /// @brief 读取一行；EOF 返回 nullopt 且 eof() 为真；超长行返回 nullopt 并设 error。
    std::optional<std::string> receive_line(std::size_t max_bytes = 64u << 20);

    /// @brief 停止本端双向通信；POSIX 保留 fd，Windows 取消并等待 I/O 后关闭管道句柄以通知对端 EOF。
    void shutdown();
    /// @brief 关闭本端；幂等，不清空缓冲与错误状态。
    void close();

    bool eof() const { return eof_; }
    const std::string& error() const { return error_; }

private:
    struct Transfer {
        enum class Kind { bytes, interrupted, closed, failed };
        Kind kind;
        std::size_t count = 0;
        std::string error;
    };
    Transfer send_bytes(std::string_view bytes);
    Transfer receive_bytes(std::span<char> bytes);

    std::unique_ptr<detail::EndpointState> native_;
    std::string buffer_;
    std::string error_;
    bool eof_ = false;
    friend std::optional<BackendProcess> spawn_backend(std::string& error);
};

/// @brief 本进程创建的后端进程；显式回收由 wait_or_terminate 执行。
/// Windows 最后一个共享进程状态销毁时关闭生命周期 Job，终止其中尚存的后端及后代。
class BackendProcess {
public:
    BackendProcess() noexcept;
    BackendProcess(BackendProcess&& other) noexcept;
    BackendProcess& operator=(BackendProcess&& other) noexcept;
    ~BackendProcess();
    BackendProcess(const BackendProcess&) = delete;
    BackendProcess& operator=(const BackendProcess&) = delete;

    Channel channel;

private:
    struct State;
    BackendProcess(Channel channel, std::shared_ptr<const State> state);
    std::shared_ptr<const State> state_;
    bool reaped_ = false;
    friend std::optional<BackendProcess> spawn_backend(std::string& error);
    friend bool wait_or_terminate(BackendProcess& process, std::chrono::milliseconds grace);
};

/// @brief 同安装目录下的 dagent-backend（Windows 为 .exe）；找不到返回空路径。
std::filesystem::path backend_binary_path();

/// @brief 创建私有连接并直接启动后端（同目录、独立进程组、只继承 stdio 和 IPC 端点）。
/// 失败返回 nullopt 并写 error。
std::optional<BackendProcess> spawn_backend(std::string& error);

/// @brief 宽限期内等待退出，POSIX 超时发 SIGTERM/SIGKILL，Windows 终止生命周期 Job。
/// 强制终止或等待失败返回 false；已结束或在宽限期退出返回 true。
bool wait_or_terminate(BackendProcess& process, std::chrono::milliseconds grace);

} // namespace dagent::ipc
