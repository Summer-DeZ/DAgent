#include "agent/host.hpp"

#include <algorithm>
#include <utility>

namespace dagent::agent {

std::shared_ptr<AgentHost> AgentHost::create(std::vector<mcp::ServerConfig> servers, mcp::Options mcp,
                                             workspace::Environment env,
                                             std::vector<SubagentDef> subagents,
                                             std::map<std::string, ProviderConfig> models) {
    // 不能 make_shared：构造函数私有。
    return std::shared_ptr<AgentHost>(new AgentHost(std::make_shared<McpHub>(std::move(servers), std::move(mcp)),
                                                    std::move(env), std::move(subagents), std::move(models)));
}

AgentHost::AgentHost(std::shared_ptr<McpHub> hub, workspace::Environment env,
                     std::vector<SubagentDef> subagents,
                     std::map<std::string, ProviderConfig> models)
    : hub_(std::move(hub)), env_(std::move(env)), subagents_(std::move(subagents)),
      models_(std::move(models)) {}

AgentHost::~AgentHost() = default;

Decision AgentHost::approve(const Approval& approval, const Approver& approver, std::stop_token stop) {
    if (!approver || stop.stop_requested()) return Decision{Decision::Answer::deny, {}, false};
    const std::lock_guard lock(approval_mutex_); // 等待期间其他子 Agent 继续跑自己的工具
    if (stop.stop_requested()) return Decision{Decision::Answer::deny, {}, false};
    return approver(approval, stop);
}

const SubagentDef* AgentHost::find_subagent(std::string_view name) const {
    const auto it = std::ranges::find_if(subagents_, [&](const SubagentDef& def) {
        return def.name == name;
    });
    return it == subagents_.end() ? nullptr : &*it;
}

} // namespace dagent::agent
