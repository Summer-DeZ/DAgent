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

/// @brief 资源分类：策略判定与工具准备共用同一语义，避免两处各写一套敏感名规则。
enum class ResourceClass {
    normal,    ///< 普通用户数据
    sensitive, ///< 敏感用户数据（.env、*.pem、.ssh、.gnupg 等）
    guarded,   ///< 执行控制面（代理配置/数据/日志/运行目录、.git 内部）
    outside,   ///< 工作区外
};

ResourceClass classify_resource(const std::filesystem::path& path, bool inside_workspace,
                                const std::filesystem::path& control_root);

std::string_view to_string(PermissionMode); ///< "ask" / "workspace" / "unrestricted"

/// @brief 生效权限：本会话设置与父级上限合成后的实际结果。
struct EffectivePermission {
    PermissionMode mode = PermissionMode::workspace;
    bool read_only = false;
    bool planning = false;
};

/// @brief a 是否严格窄于 b（规划/只读优先，其次模式）。
bool narrower(const EffectivePermission& a, const EffectivePermission& b);

/// @brief 一次权限决策的结果。
struct Verdict {
    enum class Kind { allow, ask, deny };
    Kind kind = Kind::deny;
    ExecutionGrant grant; ///< allow 时有效
    Approval approval;    ///< ask 时有效：reason、requests、session_rule 已填好
    std::string reason;   ///< deny 时有效，写入给模型的策略拒绝说明
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

    struct AuthorityView {
        EffectivePermission permission;
        std::uint64_t revision = 0;
        std::stop_token stop;
        bool can_review() const {
            return permission.mode == PermissionMode::unrestricted &&
                   !permission.read_only && !permission.planning && !stop.stop_requested();
        }
    };
    AuthorityView authority_view() const;
    bool valid_approval(const Approval&, const Decision&) const;

    /// @brief 原子校验当前 revision、保存会话规则并消费批准；陈旧批准不产生授权。
    std::optional<ExecutionGrant> consume_approval(const Approval&, const Decision&);

    /// @brief 权限修订号：模式、会话规则等任何有效范围变化都会递增。
    std::uint64_t revision() const;

    struct NetworkDecision {
        enum class Kind { allow, ask, deny };
        Kind kind = Kind::ask;
        std::string reason; ///< deny/ask 时的说明
    };
    /// @brief 运行中网络目标的当前判定：配置拒绝优先，其次配置允许与会话规则，最后才需要审批。
    NetworkDecision check_network(const NetworkTarget&) const;
    /// @brief 原子消费运行中网络批准，allow_session 时保存 host:port 精确规则。
    bool consume_network_approval(const NetworkTarget&, const Approval&, const Decision&);
    /// @brief 审批者明确拒绝过的目标：本会话内不再弹窗，保留拒绝来源。
    void remember_denied_network(const NetworkTarget&, ApprovalAuthority authority = ApprovalAuthority::user);

    void set_mode(PermissionMode mode);
    PermissionMode mode() const;
    void set_read_only(bool value);
    bool read_only() const;
    void set_planning(bool value);
    bool planning() const;

    /// @brief 生效权限：父级上限激活时取更严者；子会话只能收窄。
    EffectivePermission effective() const;
    /// @brief 设置父级权限上限（子会话用）；返回设置后的生效权限并递增 revision。
    /// 上限只与本会话派生值取更严者，父级之后放宽也不会扩大已派生的子权限。
    EffectivePermission set_parent_cap(PermissionMode mode, bool read_only, bool planning);

    struct SessionGrant { std::string id, description; };
    std::vector<SessionGrant> session_grants() const;
    bool revoke(std::string_view id);

private:
    enum class PathClass { normal, sensitive, outside, guarded };

    PathClass classify(const ResourceIntent&) const;
    bool inside_dir(const std::filesystem::path&, const std::filesystem::path&) const;
    /// 命中会话授权时返回 allow_network（exec 之外恒为 false），未命中返回 nullopt。
    std::optional<bool> matches_session(const Approval&, const PreparedIntent&) const;
    bool matches_parent_session(const Approval& saved, const Approval& requested) const;
    void remember_locked(const Approval&, const Decision&);
    void remember_network_locked(const NetworkTarget&, const Approval&);
    ExecutionGrant grant_for(const Approval&, const Decision&) const;
    ExecutionGrant grant_for_exec(SandboxProfile, GrantSource) const;
    ExecutionGrant grant_for_files(const PreparedIntent&, GrantSource) const;
    ExecutionGrant grant_for_external(const PreparedIntent&, GrantSource) const;

    std::atomic<PermissionMode> mode_;
    std::atomic<bool> read_only_;
    std::atomic<bool> planning_;
    std::atomic<bool> cap_active_{false};
    std::atomic<PermissionMode> cap_mode_{PermissionMode::unrestricted};
    std::atomic<bool> cap_read_only_{false};
    std::atomic<bool> cap_planning_{false};
    std::atomic<std::uint64_t> revision_{0};
    mutable std::mutex rules_mutex_; ///< 保护下面的规则容器；evaluate/remember/revoke/快照共用
    SandboxSupport sandbox_;
    SandboxConfig sandbox_options_;
    int analysis_version_;
    std::filesystem::path workspace_root_, project_root_, control_root_;

    std::stop_source authority_stop_;
    void change_permission(const std::function<void()>& change);
    struct RememberedApproval { Approval approval; Decision decision; };
    std::vector<RememberedApproval> parent_grants_;
    bool session_edits_ = false;
    struct ExecRule {
        std::string id, command, cwd;
    };
    struct NetworkRule {
        std::string id, host;
        int port = 0;
        ApprovalAuthority authority = ApprovalAuthority::user;
    };
    std::vector<ExecRule> exec_rules_;
    struct ParentNetworkRule {
        NetworkTarget target;
        Approval approval;
    };
    std::vector<ParentNetworkRule> parent_network_rules_;
    std::vector<NetworkRule> network_rules_;
    std::vector<NetworkRule> network_denied_;
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
