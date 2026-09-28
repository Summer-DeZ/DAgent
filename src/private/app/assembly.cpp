#include "app/assembly.hpp"

#include <algorithm>
#include <utility>

namespace dagent::app {

std::shared_ptr<Assembly> Assembly::create(
    std::vector<mcp::ServerConfig> servers, mcp::Options mcp, workspace::Environment env,
    std::vector<agent::SubagentDef> subagents) {
    // 不能 make_shared：构造函数私有。
    return std::shared_ptr<Assembly>(new Assembly(
        std::make_shared<tools::McpHub>(std::move(servers), std::move(mcp)), std::move(env),
        std::move(subagents)));
}

Assembly::Assembly(std::shared_ptr<tools::McpHub> hub, workspace::Environment env,
                   std::vector<agent::SubagentDef> subagents)
    : hub_(std::move(hub)), env_(std::move(env)), subagents_(std::move(subagents)) {}

Assembly::~Assembly() = default;

const agent::SubagentDef* Assembly::find_subagent(std::string_view name) const {
    const auto it = std::ranges::find_if(subagents_, [&](const agent::SubagentDef& def) {
        return def.name == name;
    });
    return it == subagents_.end() ? nullptr : &*it;
}

} // namespace dagent::app
