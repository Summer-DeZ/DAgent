/// @file subagent.hpp
/// @brief SubagentExecutor：一次 task 委派的完整执行与资源回收（R08）。
///
/// 在 task 组线程创建子 Session/Run，父等待结果；子审批经父审批出口（InteractionBroker）
/// 串行显示，子 Asker 恒空，子工具默认不含 task/ask/exit_plan 与未显式允许的 MCP。
#pragma once

#include <string>

#include "agent/port_delegation.hpp"
#include "runtime/factory.hpp"

namespace dagent::runtime {

class SubagentExecutor final : public agent::DelegationChannel {
public:
    explicit SubagentExecutor(SessionFactory& factory) : factory_(factory) {}
    ~SubagentExecutor() override = default;

    agent::ToolResult delegate(const agent::DelegationContext&, const agent::DelegationRequest&) override;

private:
    SessionFactory& factory_;
};

} // namespace dagent::runtime
