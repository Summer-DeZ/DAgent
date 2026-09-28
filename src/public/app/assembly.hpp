/// @file assembly.hpp
/// @brief 一次前端/后端运行共享的环境：MCP 连接、工作区事实、子 Agent 定义。
///
/// 由启动装配创建一次，比所有 Session 活得久。审批的单模态排队由 runtime/InteractionBroker 负责。
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "agent/subagent_def.hpp"
#include "mcp/client.hpp"
#include "tools/mcp_hub.hpp"
#include "workspace/context.hpp"

namespace dagent::app {

class Assembly {
public:
    static std::shared_ptr<Assembly> create(
        std::vector<mcp::ServerConfig> servers, mcp::Options mcp, workspace::Environment env,
        std::vector<agent::SubagentDef> subagents);
    ~Assembly();
    Assembly(const Assembly&) = delete;
    Assembly& operator=(const Assembly&) = delete;

    const std::shared_ptr<tools::McpHub>& hub() const { return hub_; }
    const workspace::Environment& environment() const { return env_; }
    const std::vector<agent::SubagentDef>& subagents() const { return subagents_; }
    const agent::SubagentDef* find_subagent(std::string_view name) const;

private:
    Assembly(std::shared_ptr<tools::McpHub>, workspace::Environment, std::vector<agent::SubagentDef>);

    std::shared_ptr<tools::McpHub> hub_;
    workspace::Environment env_;
    std::vector<agent::SubagentDef> subagents_;
};

} // namespace dagent::app
