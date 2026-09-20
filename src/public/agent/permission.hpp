/// @file permission.hpp
/// @brief 权限策略：按 Intent 判定允许 / 询问 / 拒绝，以及允许时给 bash 什么沙箱。
///
/// 纯逻辑：不读文件、不弹对话框；需要询问时由调度器调 Approver（docs/design/agent.md §6）。
#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/events.hpp"
#include "agent/message.hpp"
#include "exec/sandbox.hpp"
#include "tools/tools.hpp"
#include "workspace/files.hpp"

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
    tools::Grant grant;    ///< allow 时有效
    Approval approval;     ///< ask 时有效：reason、session_rule、can_network 已填好
    std::string reason;    ///< deny 时有效，进 T6
};

/// @brief 按模式与规则表判定。线程安全：set_mode 是 atomic，下一次决策生效。
class Policy {
public:
    Policy(PermissionMode mode, bool read_only, bool planning, exec::Support sandbox,
           std::filesystem::path workspace_root, std::filesystem::path project_root);

    Verdict evaluate(const ToolCall&, const tools::Intent&) const;

    /// @brief allow_session 时记下规则（本会话授权，只在内存里）。
    void remember(const Approval&, const Decision&);
    tools::Grant grant_for(const Approval&, const Decision&) const;

    void set_mode(PermissionMode mode);
    PermissionMode mode() const;
    void set_read_only(bool value);
    bool read_only() const;
    void set_planning(bool value);
    bool planning() const;

private:
    enum class PathClass { normal, sensitive, outside, guarded };

    PathClass classify(const workspace::Resolved&) const;
    bool inside_dir(const std::filesystem::path&, const std::filesystem::path&) const;
    /// 命中会话授权时返回 allow_network（exec 之外恒为 false），未命中返回 nullopt。
    std::optional<bool> matches_session(const Approval&, const tools::Intent&) const;
    tools::Grant grant_for_exec() const;

    std::atomic<PermissionMode> mode_;
    std::atomic<bool> read_only_;
    std::atomic<bool> planning_;
    exec::Support sandbox_;
    std::filesystem::path workspace_root_, project_root_;

    bool session_edits_ = false;
    struct ExecRule {
        std::string prefix;
        bool network = false;
    };
    std::vector<ExecRule> exec_rules_;
    std::vector<std::string> external_rules_;
    std::vector<std::filesystem::path> read_dirs_;
};

/// @brief docs/design/agent.md §6：只有直接放行的只读调用能进并行组。
bool parallel(const Verdict&, const tools::Intent&);

} // namespace dagent::agent
