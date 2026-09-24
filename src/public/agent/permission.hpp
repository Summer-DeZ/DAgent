/// @file permission.hpp
/// @brief 权限策略：按 PreparedIntent 判定允许 / 询问 / 拒绝，以及允许时给 bash 什么沙箱。
///
/// 纯逻辑：不读文件、不弹对话框；需要询问时由调度器调 Approver（docs/design/agent.md §6）。
/// 输入是中立意图摘要与沙箱支持值；exec 分析、workspace 解析都发生在工具准备阶段。
#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/events.hpp"
#include "agent/grant.hpp"
#include "agent/intent.hpp"
#include "agent/message.hpp"

namespace dagent::agent {

enum class PermissionMode {
    ask,
    workspace,
    unrestricted,
};

std::string_view to_string(PermissionMode); ///< "ask" / "workspace" / "unrestricted"

/// @brief 一次权限决策的结果。
struct Verdict {
    enum class Kind { allow, ask, deny };
    Kind kind = Kind::deny;
    ExecutionGrant grant; ///< allow 时有效
    Approval approval;    ///< ask 时有效：reason、session_rule、can_network 已填好
    std::string reason;   ///< deny 时有效，进 T6
};

/// @brief 按模式与规则表判定。线程安全：规则容器与模式快照由同一把短锁保护，
/// 锁内不调用用户交互、模型、存储或 socket；下一次决策生效。
class Policy {
public:
    Policy(PermissionMode mode, bool read_only, bool planning, SandboxSupport sandbox,
           SandboxConfig sandbox_options, int analysis_version,
           std::filesystem::path workspace_root, std::filesystem::path project_root,
           std::filesystem::path control_root);

    Verdict evaluate(const ToolCall&, const PreparedIntent&) const;

    /// @brief allow_session 时记下规则（本会话授权，只在内存里）。
    void remember(const Approval&, const Decision&);
    ExecutionGrant grant_for(const Approval&, const Decision&) const;

    void set_mode(PermissionMode mode);
    PermissionMode mode() const;
    void set_read_only(bool value);
    bool read_only() const;
    void set_planning(bool value);
    bool planning() const;

    struct SessionGrant { std::string id, description; };
    std::vector<SessionGrant> session_grants() const;
    bool revoke(std::string_view id);

private:
    enum class PathClass { normal, sensitive, outside, guarded };

    PathClass classify(const ResourceIntent&) const;
    bool inside_dir(const std::filesystem::path&, const std::filesystem::path&) const;
    /// 命中会话授权时返回 allow_network（exec 之外恒为 false），未命中返回 nullopt。
    std::optional<bool> matches_session(const Approval&, const PreparedIntent&) const;
    ExecutionGrant grant_for_exec(SandboxProfile, GrantSource) const;

    std::atomic<PermissionMode> mode_;
    std::atomic<bool> read_only_;
    std::atomic<bool> planning_;
    mutable std::mutex rules_mutex_; ///< 保护下面的规则容器；evaluate/remember/revoke/快照共用
    SandboxSupport sandbox_;
    SandboxConfig sandbox_options_;
    int analysis_version_;
    std::filesystem::path workspace_root_, project_root_, control_root_;

    bool session_edits_ = false;
    struct ExecRule {
        std::string id, command, cwd;
    };
    std::vector<ExecRule> exec_rules_;
    std::vector<std::string> external_rules_;
    std::vector<std::filesystem::path> read_dirs_;
};

/// @brief docs/design/agent.md §6：只有直接放行的只读调用能进并行组。
bool parallel(const Verdict&, const PreparedIntent&);

/// @brief 子 Agent 的权限派生结果。
struct DerivedPermission {
    PermissionMode mode = PermissionMode::workspace;
    bool read_only = false;
    bool planning = false;
    bool may_ask = false; ///< false → 子 Agent 的 RunServices 不带审批出口
};

/// @brief 子 Agent 的权限派生：只能收窄不能放宽。
/// parent_* 必须取父 Policy 的运行时当前值（policy_.mode() / planning() / read_only()），
/// 不能取装配初值——用户可能按过 Shift+Tab，或走过 exit_plan 切换了模式。
DerivedPermission derive_permission(PermissionMode parent_mode, bool parent_planning,
                                    bool parent_read_only, std::string_view def_permission);

} // namespace dagent::agent
