/// @file srt.hpp
/// @brief SRT 后端执行：每个 Bash 实例一个 Node bridge，命令在 bubblewrap + 代理出口内运行。
///
/// 这里只做“中立 Policy → SRT 配置 → 进程与文件规则”的转换和生命周期管理：
/// bridge 负责 SRT 自身的初始化、清理与占位点回收，C++ 侧不解析 stderr 推测后端状态。
/// 网络目标只使用已批准列表（空表示全部拒绝）；运行中审批由上层调度接入。
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "exec/process.hpp"
#include "exec/sandbox.hpp"

namespace dagent::exec {

/// 运行中网络目标的判定结果。
enum class NetworkGateResult { allow, deny, cancel };

/// 由调度层提供的运行中网络判定；为空表示拒绝全部请求。
using NetworkGate = std::function<NetworkGateResult(std::string_view host, int port, std::string& reason, std::stop_token)>;

/// @brief internal/sandbox 环境里的绝对路径；启动装配时解析一次。
struct SrtRuntime {
    std::filesystem::path node;
    std::filesystem::path bridge; ///< libexec/srt_bridge.mjs
    std::filesystem::path entry;  ///< @anthropic-ai/sandbox-runtime/dist/index.js
    std::filesystem::path bwrap;
    std::filesystem::path socat;
    std::filesystem::path rg;
    std::filesystem::path shell; ///< 受限命令使用的 managed bash
};

/// @brief 一次 SRT 受限执行的输入。文件规则来自中立 Policy，网络只接受已批准 host:port。
struct SrtRequest {
    SrtRuntime runtime;
    std::filesystem::path workspace;  ///< 命令工作目录；bridge 在命令前 cd 到这里
    std::filesystem::path state_root; ///< 每执行私有目录（控制目录与私有 HOME）的父目录
    std::string command;
    Policy policy;
    /// 命令环境（PATH、LC_ALL 等）；HOME/TMPDIR 系列由本函数指向私有目录。
    std::vector<std::pair<std::string, std::string>> environment;
    std::optional<std::chrono::milliseconds> timeout;
    std::chrono::milliseconds approval_timeout{120000};
    int max_network_requests = 32;
    NetworkGate network_gate; ///< 运行中网络目标判定；为空时全部拒绝
};

/// @brief 在 SRT 后端执行一条命令。语义与 run() 一致；bridge/后端失败抛 ExecError{sandbox}。
Result run_srt(const SrtRequest& request, const Options& options = {},
               const std::function<void(Stream, std::string_view)>& on_output = {},
               std::stop_token stop = {});

/// Shared read-only execution boundary for search and workspace context commands.
struct ReadOnlySandbox {
    std::optional<SrtRuntime> runtime;
    std::filesystem::path state_root;
    std::vector<std::filesystem::path> protected_read;
    std::vector<std::filesystem::path> read_exceptions;
    Result run(const Command&, const Options& = {},
               const std::function<void(Stream, std::string_view)>& = {}, std::stop_token = {}) const;
};

} // namespace dagent::exec
