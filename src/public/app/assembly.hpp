/// @file assembly.hpp
/// @brief 一次前端/后端运行共享的环境：MCP 连接、工作区事实、子 Agent 定义与模型表。
///
/// 由启动装配创建一次，比所有 Session 活得久。审批的单模态排队由 runtime/InteractionBroker
/// 负责，本对象不再仲裁交互（R07/R08）。
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string_view>
#include <vector>

#include "agent/port_model.hpp"
#include "agent/subagent_def.hpp"
#include "llm/provider.hpp"
#include "mcp/client.hpp"
#include "tools/mcp_hub.hpp"
#include "tools/tools.hpp"
#include "workspace/context.hpp"

namespace dagent::app {

class Assembly {
public:
    /// model_factory 由 llm 模块在装配层注入；凭据只进入 llm 内部配置。
    static std::shared_ptr<Assembly> create(
        std::vector<mcp::ServerConfig> servers, mcp::Options mcp, workspace::Environment env,
        std::vector<agent::SubagentDef> subagents, std::map<std::string, llm::ProviderConfig> models,
        std::function<std::shared_ptr<agent::ModelSession>(const llm::ProviderConfig&)> model_factory);
    ~Assembly();
    Assembly(const Assembly&) = delete;
    Assembly& operator=(const Assembly&) = delete;

    /// MCP：主会话在步骤边界重连与等待；子会话用 snapshot 取连接快照。
    const std::shared_ptr<tools::McpHub>& hub() const { return hub_; }

    /// 启动时收集一次的环境事实；所有会话渲染 system prompt 都复用它，不再跑 git。
    const workspace::Environment& environment() const { return env_; }

    const std::vector<agent::SubagentDef>& subagents() const { return subagents_; }
    const agent::SubagentDef* find_subagent(std::string_view name) const;
    const std::map<std::string, llm::ProviderConfig>& models() const { return models_; }

    /// 按内部配置构造一个已配置的模型客户端（子 Agent 模型选择用）。
    std::shared_ptr<agent::ModelSession> make_model_session(const llm::ProviderConfig&) const;

private:
    Assembly(std::shared_ptr<tools::McpHub>, workspace::Environment, std::vector<agent::SubagentDef>,
             std::map<std::string, llm::ProviderConfig>,
             std::function<std::shared_ptr<agent::ModelSession>(const llm::ProviderConfig&)>);

    std::shared_ptr<tools::McpHub> hub_;
    workspace::Environment env_;
    std::vector<agent::SubagentDef> subagents_;
    std::map<std::string, llm::ProviderConfig> models_;
    std::function<std::shared_ptr<agent::ModelSession>(const llm::ProviderConfig&)> model_factory_;
};

} // namespace dagent::app
