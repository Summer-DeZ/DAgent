#include "app/assembly.hpp"

#include <algorithm>
#include <utility>

namespace dagent::app {

std::shared_ptr<Assembly> Assembly::create(
    std::vector<mcp::ServerConfig> servers, mcp::Options mcp, workspace::Environment env,
    std::vector<agent::SubagentDef> subagents, std::map<std::string, llm::ProviderConfig> models,
    std::function<std::shared_ptr<agent::ModelSession>(const llm::ProviderConfig&)> model_factory) {
    // 不能 make_shared：构造函数私有。
    return std::shared_ptr<Assembly>(new Assembly(
        std::make_shared<tools::McpHub>(std::move(servers), std::move(mcp)), std::move(env),
        std::move(subagents), std::move(models), std::move(model_factory)));
}

Assembly::Assembly(std::shared_ptr<tools::McpHub> hub, workspace::Environment env,
                   std::vector<agent::SubagentDef> subagents,
                   std::map<std::string, llm::ProviderConfig> models,
                   std::function<std::shared_ptr<agent::ModelSession>(const llm::ProviderConfig&)> model_factory)
    : hub_(std::move(hub)), env_(std::move(env)), subagents_(std::move(subagents)),
      models_(std::move(models)), model_factory_(std::move(model_factory)) {}

Assembly::~Assembly() = default;

const agent::SubagentDef* Assembly::find_subagent(std::string_view name) const {
    const auto it = std::ranges::find_if(subagents_, [&](const agent::SubagentDef& def) {
        return def.name == name;
    });
    return it == subagents_.end() ? nullptr : &*it;
}

std::shared_ptr<agent::ModelSession> Assembly::make_model_session(const llm::ProviderConfig& provider) const {
    return model_factory_(provider);
}

} // namespace dagent::app
