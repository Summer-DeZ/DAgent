/// @file sandbox.hpp
/// @brief 受限执行的中立值：模式、路径/网络边界与能力快照。
///
/// 实际隔离由 MXC 后端（exec/mxc.hpp）落实；这里不包含任何内核规则实现。
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace dagent::exec {

/// 三档权限，和 codex 对应。
enum class Mode { read_only, workspace_write, full_access };

struct SandboxOptions {
    int version = 3;
    /// 用户维护的持久读取/写入范围；相对路径由配置层按 workspace 解析。
    std::vector<std::filesystem::path> extra_readable;
    std::vector<std::filesystem::path> extra_writable;
    /// 持久网络目标（`host` 或 `host:port`）；denied 优先。
    std::vector<std::string> network_allowed;
    std::vector<std::string> network_denied;
    std::string backend = "mxc";          ///< 执行后端；唯一受支持值是 "mxc"
    std::string host_access = "ask_once"; ///< 能力不足时的宿主访问提示策略
    std::chrono::milliseconds network_approval_timeout{120000}; ///< 一次运行中网络审批的等待上限
    int max_network_requests_per_execution = 32;              ///< 每次执行的运行中网络请求预算
};

struct Policy {
    Mode mode = Mode::workspace_write;
    /// 可读/可写路径均为明确授权。
    std::vector<std::filesystem::path> readable;
    std::vector<std::filesystem::path> writable;
    /// writable/readable 中的子树不能重新放行这些路径。
    std::vector<std::filesystem::path> protected_read;
    std::vector<std::filesystem::path> read_exceptions;
    std::vector<std::filesystem::path> protected_write;
    bool protect_sensitive_names = true;
    /// 已批准的 host:port 网络目标；受限后端只能放行这些目标（空表示全部拒绝）。
    std::vector<std::string> network_targets;
};

/// 运行中网络目标的判定结果。
enum class NetworkGateResult { allow, deny, cancel };

/// 由调度层提供的运行中网络判定；为空表示拒绝全部请求。
using NetworkGate = std::function<NetworkGateResult(std::string_view host, int port, std::string& reason, std::stop_token)>;

/// @brief 枚举 root 下已存在的敏感路径（.env、*.pem、*.key、id_*、.ssh、.gnupg）。
/// root 为单个文件时仅检查该文件的敏感名，不进行目录遍历。
/// 执行后端用它把保护落实到显式拒绝规则；不存在的名字不返回，避免制造占位点。
std::vector<std::filesystem::path> sensitive_paths(const std::filesystem::path& root);

/// @brief 启动时真实探测一次：没有可用沙箱时核心要降级为「必须询问用户」。
struct Support {
    std::string backend = "none"; ///< "mxc" 或 "none"
    /// 受限命令能向自己的子孙发信号。
    bool child_signals = false;
    std::vector<std::string> missing;

    /// 动态 workspace 命令要求的完整边界；已知只读只要求 read_only_ready()。
    bool read_only_ready() const;
    bool workspace_ready() const;
};

} // namespace dagent::exec
