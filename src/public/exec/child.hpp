/// @file child.hpp
/// @brief 长期存活的子进程：stdin/stdout 走管道，stderr 逐行写日志。给 MCP 的 stdio 传输使用。
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "exec/process.hpp"

namespace dagent::exec {

/// @brief 内部有一个读取线程跑 Asio 的 io_context；on_line/on_exit 都在这个线程上触发。
class Child {
public:
    /// @brief 启动子进程；同样 setsid、过滤环境变量，stdin 接管道。失败抛 ExecError{spawn_failed}。
    static std::unique_ptr<Child> spawn(const Command& cmd, const Options& opt = {});

    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child();

    /// @brief 往 stdin 写一段数据；线程安全。子进程已退出或积压过多时丢弃并记日志。
    void write(std::string_view data);

    /// @brief stdout 每收到一行调用一次（行不含换行符）。
    void on_line(std::function<void(std::string_view)> cb);

    /// @brief stderr 每收到一行调用一次（行不含换行符）；未注册时按日志输出。
    /// 注册前到达的少量行会补发，供 bridge 握手帧这类早于回调的少量状态使用。
    void on_stderr(std::function<void(std::string_view)> cb);

    /// @brief 进程退出时调用一次；已经退出后注册会立即补发。正常情况下 code/signal 恰有一个有值，
    /// 等待子进程失败（极少见，会记日志）时两者都为空。
    void on_exit(std::function<void(std::optional<int> code, std::optional<int> signal)> cb);

    /// @brief 先对进程组发 SIGTERM，kill_grace 后 SIGKILL，并等待读取线程结束；可重复调用。
    void terminate();

private:
    Child();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dagent::exec
