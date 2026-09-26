/// @file grant.hpp
/// @brief 执行授权与沙箱支持能力的中立值。
///
/// 沙箱 profile 在 tools/exec 适配处映射为执行层的 backend/mode。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace dagent::agent {

/// bash 沙箱的三档边界；与 exec::Mode 对应。
enum class SandboxProfile { read_only, workspace_write, full_access };

/// 授权来源。
enum class GrantSource { mode, once, session, unrestricted };

/// @brief 一次执行的决定，授权后传回工具执行。
struct ExecutionGrant {
    SandboxProfile sandbox = SandboxProfile::workspace_write; ///< 只对 bash 有意义
    bool allow_network = false;
    bool allow_local_sockets = false;
    bool private_tmp = true;
    bool protect_sensitive_names = false;
    GrantSource source = GrantSource::mode;
    std::string backend;
    int analysis_version = 0;
    std::vector<std::filesystem::path> readable;
    std::vector<std::filesystem::path> writable;
    std::vector<std::filesystem::path> protected_read;
    std::vector<std::filesystem::path> protected_write;
    std::vector<std::string> network_targets;
};

/// @brief 沙箱支持能力：启动时探测一次的中立事实；选型规则由策略按这些布尔值执行。
struct SandboxSupport {
    std::string backend = "none";
    bool read_only_ready = false;   ///< 已知只读命令要求的边界
    bool workspace_ready = false;   ///< 动态 workspace 命令要求的完整边界
    std::vector<std::string> missing;
};

/// @brief 沙箱配置值：用户维护的持久读取/写入范围（相对路径由配置层解析）。
struct SandboxConfig {
    int version = 1;
    std::vector<std::filesystem::path> extra_readable;
    std::vector<std::filesystem::path> extra_writable;
};

std::string_view to_string(SandboxProfile);          ///< "read_only" / "workspace_write" / "full_access"
std::string_view to_string(GrantSource);             ///< "mode" / "once" / "session" / "unrestricted"

} // namespace dagent::agent
