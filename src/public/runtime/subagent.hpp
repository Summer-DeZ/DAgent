/// @file subagent.hpp
/// @brief SubagentExecutor：一次 task 委派的完整执行与资源回收。
///
/// 在 task 组线程创建子 Session/Run，父等待结果并消费审批邮箱；unrestricted 父优先代审，
/// 其他模式走用户审批。子 Asker 恒空，子工具默认不含 task/ask/exit_plan 与未显式允许的 MCP。
/// 活跃子会话登记各自的权限上限与取消入口：父降权时即时收窄并终止越界子执行。
#pragma once

#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include "agent/permission.hpp"
#include "agent/port_delegation.hpp"
#include "runtime/factory.hpp"

namespace dagent::runtime {

class SubagentExecutor final : public agent::DelegationChannel {
public:
    explicit SubagentExecutor(SessionFactory& factory) : factory_(factory) {}
    ~SubagentExecutor() override = default;

    agent::ToolResult delegate(const agent::DelegationContext&, const agent::DelegationRequest&) override;

    /// @brief 父权限上限变化：收窄活跃子会话；生效权限变窄的子执行被请求停止。
    void apply_parent_permission(agent::PermissionMode mode, bool read_only, bool planning);

    /// @brief 登记一个活跃子会话；返回其取消入口（子 Run 使用同一个 stop 来源）。
    std::shared_ptr<std::stop_source> attach(agent::Policy& policy,
                                            const agent::DelegationContext& context);
    void detach(agent::Policy& policy);

private:
    struct ActiveChild {
        agent::Policy* policy = nullptr;
        std::shared_ptr<std::stop_source> stop;
        agent::EffectivePermission effective;
        const agent::Policy* parent_policy = nullptr;
        std::uint64_t parent_revision = 0;
        bool parent_review = false;
    };

    SessionFactory& factory_;
    std::mutex mutex_;
    std::vector<ActiveChild> children_;
};

} // namespace dagent::runtime
