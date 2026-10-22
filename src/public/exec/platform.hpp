/// @file platform.hpp
/// @brief exec 模块内部的平台边界；共享实现保留异步调度、计时器与协议处理。
#pragma once

#include <memory>
#include <span>

#include <boost/asio/io_context.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/process/v2/process.hpp>

#include "exec/process.hpp"

namespace dagent::exec::detail {

class ProcessGroup {
public:
    ProcessGroup();
    ~ProcessGroup();
    ProcessGroup(const ProcessGroup&) = delete;
    ProcessGroup& operator=(const ProcessGroup&) = delete;

    /// Windows 加入生命周期 Job 失败时抛 ExecError；调用时子进程仍挂起。
    void capture(const boost::process::v2::process& process);
    bool valid() const noexcept;
    void terminate() noexcept;
    void force_kill() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct ExitStatus {
    std::optional<int> code;
    std::optional<int> signal;
};

ExitStatus process_exit_status(const boost::process::v2::process& process) noexcept;

void spawn_command(const Command& cmd, const std::filesystem::path& program,
                   const std::vector<std::string>& environment,
                   boost::asio::io_context& context, boost::process::v2::process& process,
                   boost::asio::writable_pipe& input, boost::asio::readable_pipe& output,
                   boost::asio::readable_pipe& error, ProcessGroup& group, bool& spawned);

void spawn_child(const Command& cmd, const std::filesystem::path& program,
                 const std::vector<std::string>& environment,
                 boost::asio::io_context& context, boost::process::v2::process& process,
                 boost::asio::writable_pipe& input, boost::asio::readable_pipe& output,
                 boost::asio::readable_pipe& error, ProcessGroup& group);

/// 动态库句柄：MXC 原生库在运行时加载，不在链接期依赖。
class SharedLibrary {
public:
    /// 失败抛 ExecError{sandbox}，消息含文件名与平台错误文本。
    static SharedLibrary open(const std::filesystem::path& file);
    SharedLibrary(SharedLibrary&&) noexcept;
    SharedLibrary& operator=(SharedLibrary&&) noexcept;
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;
    ~SharedLibrary();

    /// 缺失的符号返回 nullptr。
    void* symbol(const char* name) const noexcept;

private:
    SharedLibrary();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 受限命令的 MXC `process.commandLine`：POSIX 为 `exec '<shell>' --noprofile --norc -c '<script>'`
/// （sh 单引号转义，由 /bin/sh -c 解释）；Windows 按 CreateProcess 的引用规则拼出
/// `"<shell>" --noprofile --norc -c "<script>"`。
std::string sandbox_command_line(const std::filesystem::path& shell, std::string_view script);

/// 127.0.0.1 上的入站连接对端是否属于当前用户：Linux 按 /proc/net/tcp 的 uid 判断；Windows 第一版返回 true。
bool loopback_peer_is_current_user(unsigned short local_port, unsigned short peer_port);

} // namespace dagent::exec::detail
