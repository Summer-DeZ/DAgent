/// @file permission.hpp
/// @brief 权限策略：按 Intent 判定允许 / 询问 / 拒绝，以及允许时给 bash 什么沙箱。
///
/// 纯逻辑：不读文件、不弹对话框；需要询问时由调度器调 Approver（05-dispatch）。
#pragma once

#include <atomic>
#include <filesystem>
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
    ask,          ///< 交互界面默认：规则说「询问」就问
    accept_edits, ///< 交互界面：工作区内的普通写入不再询问
    automatic,    ///< run 模式默认（配置里写 "auto"）：「询问」变成允许，但有硬性拒绝
    deny,         ///< run 模式：只读 agent
};

std::string_view to_string(PermissionMode); ///< "ask" / "accept_edits" / "auto" / "deny"

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
    Policy(PermissionMode mode, exec::Support sandbox, std::filesystem::path workspace_root,
           std::filesystem::path project_root);

    Verdict evaluate(const ToolCall&, const tools::Intent&) const;

    /// @brief allow_session 时记下规则（本会话授权，只在内存里）。
    void remember(const Approval&, const Decision&);
    tools::Grant grant_for(const Approval&, const Decision&) const;

    void set_mode(PermissionMode mode);
    PermissionMode mode() const;

private:
    enum class PathClass { normal, sensitive, outside, guarded };

    PathClass classify(const workspace::Resolved&) const;
    bool inside_dir(const std::filesystem::path&, const std::filesystem::path&) const;
    bool matches_session(const Approval&, const tools::Intent&) const;
    tools::Grant grant_for_exec() const;

    std::atomic<PermissionMode> mode_;
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

/// @brief 05-dispatch 规则 1：只有直接放行的只读调用能进并行组。
bool parallel(const Verdict&, const tools::Intent&);

} // namespace dagent::agent
