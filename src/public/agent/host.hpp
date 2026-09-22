/// @file host.hpp
/// @brief 父子 Agent 共享的运行时：MCP 连接、环境事实、审批仲裁、子 Agent 定义表。
/// 线程安全：所有方法可从任意 Agent 线程调用。由入口创建一次，比所有 Agent 实例活得久。
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include "agent/events.hpp"
#include "agent/mcp_hub.hpp"
#include "agent/options.hpp"
#include "llm/provider.hpp"
#include "workspace/context.hpp"

namespace dagent::agent {

class AgentHost {
public:
    /// http/retry 是模型客户端装配参数；model_factory 由 llm 模块在装配层注入。
    static std::shared_ptr<AgentHost> create(std::vector<mcp::ServerConfig> servers, mcp::Options mcp,
                                             workspace::Environment env,
                                             std::vector<SubagentDef> subagents,
                                             std::map<std::string, llm::ProviderConfig> models,
                                             std::function<std::shared_ptr<ModelSession>(const llm::ProviderConfig&)>
                                                 model_factory);
    ~AgentHost();
    AgentHost(const AgentHost&) = delete;
    AgentHost& operator=(const AgentHost&) = delete;

    /// MCP：主 Agent 用 apply_pending 重连与等待，子 Agent 只用 snapshot。
    const std::shared_ptr<McpHub>& hub() const { return hub_; }

    /// 启动时收集一次的环境事实；所有 Agent 渲染 system prompt 都复用它，不再跑 git。
    const workspace::Environment& environment() const { return env_; }

    /// 审批仲裁：并发子 Agent 的请求在此排队，任一时刻只有一个对话框。
    /// approver 为空时直接返回 deny 且不取锁；stop 已触发时同样直接返回 deny。
    Decision approve(const Approval&, const Approver&, std::stop_token);

    const std::vector<SubagentDef>& subagents() const { return subagents_; }
    const SubagentDef* find_subagent(std::string_view name) const;
    const std::map<std::string, llm::ProviderConfig>& models() const { return models_; }

    /// 按内部配置构造一个已配置的模型客户端（子 Agent 模型选择用）。
    std::shared_ptr<ModelSession> make_model_session(const llm::ProviderConfig&) const;

private:
    AgentHost(std::shared_ptr<McpHub>, workspace::Environment, std::vector<SubagentDef>,
              std::map<std::string, llm::ProviderConfig>,
              std::function<std::shared_ptr<ModelSession>(const llm::ProviderConfig&)>);

    std::shared_ptr<McpHub> hub_;
    workspace::Environment env_;
    std::vector<SubagentDef> subagents_;
    std::map<std::string, llm::ProviderConfig> models_;
    std::function<std::shared_ptr<ModelSession>(const llm::ProviderConfig&)> model_factory_;
    std::mutex approval_mutex_; ///< ApprovalDialog 是单模态，并发 open 会互相覆盖
};

} // namespace dagent::agent
