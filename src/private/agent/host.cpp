#include "agent/host.hpp"

#include <algorithm>
#include <utility>

namespace dagent::agent {

std::shared_ptr<AgentHost> AgentHost::create(
    std::vector<mcp::ServerConfig> servers, mcp::Options mcp, workspace::Environment env,
    std::vector<SubagentDef> subagents, std::map<std::string, llm::ProviderConfig> models,
    std::function<std::shared_ptr<ModelSession>(const llm::ProviderConfig&)> model_factory) {
    // 不能 make_shared：构造函数私有。
    return std::shared_ptr<AgentHost>(new AgentHost(std::make_shared<McpHub>(std::move(servers), std::move(mcp)),
                                                    std::move(env), std::move(subagents), std::move(models),
                                                    std::move(model_factory)));
}

AgentHost::AgentHost(std::shared_ptr<McpHub> hub, workspace::Environment env,
                     std::vector<SubagentDef> subagents, std::map<std::string, llm::ProviderConfig> models,
                     std::function<std::shared_ptr<ModelSession>(const llm::ProviderConfig&)> model_factory)
    : hub_(std::move(hub)), env_(std::move(env)), subagents_(std::move(subagents)),
      models_(std::move(models)), model_factory_(std::move(model_factory)) {}

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

std::shared_ptr<ModelSession> AgentHost::make_model_session(const llm::ProviderConfig& provider) const {
    return model_factory_(provider);
}

} // namespace dagent::agent
