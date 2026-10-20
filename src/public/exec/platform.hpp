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

    void capture(const boost::process::v2::process& process) noexcept;
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

class SrtControl {
public:
    SrtControl();
    ~SrtControl();
    SrtControl(const SrtControl&) = delete;
    SrtControl& operator=(const SrtControl&) = delete;

    void inherit_bridge_endpoint(Command&) const;
    std::string bridge_argument() const;

    std::optional<std::string> send_initial(std::string_view);
    void send_decision(std::string_view);
    void send_start(std::string_view);
    std::size_t read_chunk(std::span<char>);

    void shutdown_host() noexcept;
    void close_bridge() noexcept;
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::vector<std::filesystem::path> default_srt_readable_paths();

} // namespace dagent::exec::detail
