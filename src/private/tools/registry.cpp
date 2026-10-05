#include "tools/tools.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "tools/detail.hpp"

namespace dagent::tools {

void Registry::add(std::unique_ptr<Tool> tool) {
    const std::string name = tool->spec().name;
    const auto existing = std::find_if(tools_.begin(), tools_.end(), [&](const auto& t) {
        return t->spec().name == name;
    });
    if (existing != tools_.end()) {
        *existing = std::move(tool); // 同名替换，保持原有位置，工具列表顺序不抖动
        return;
    }
    tools_.push_back(std::move(tool));
}

void Registry::remove_prefix(std::string_view prefix) {
    std::erase_if(tools_, [&](const std::unique_ptr<Tool>& tool) {
        return std::string_view(tool->spec().name).starts_with(prefix);
    });
}

void Registry::retain(const std::vector<std::string>& names) {
    std::erase_if(tools_, [&](const std::unique_ptr<Tool>& tool) {
        return std::ranges::find(names, tool->spec().name) == names.end();
    });
}

const Tool* Registry::find(std::string_view name) const {
    const auto it = std::find_if(tools_.begin(), tools_.end(), [&](const std::unique_ptr<Tool>& tool) {
        return std::string_view(tool->spec().name) == name;
    });
    return it == tools_.end() ? nullptr : it->get();
}

std::vector<const Spec*> Registry::specs() const {
    std::vector<const Spec*> out;
    out.reserve(tools_.size());
    for (const auto& tool : tools_) out.push_back(&tool->spec());
    return out;
}

void add_builtin(Registry& registry) {
    registry.add(detail::make_read_tool());
    registry.add(detail::make_write_tool());
    registry.add(detail::make_edit_tool());
    registry.add(detail::make_bash_tool());
    registry.add(detail::make_grep_tool());
    registry.add(detail::make_glob_tool());
    registry.add(detail::make_web_search_tool());
    registry.add(detail::make_web_fetch_tool());
}

ToolSession::ToolSession(const Registry& registry, Context& context)
    : registry_(registry), context_(context) {}

std::vector<agent::ToolSpec> ToolSession::specs() const {
    std::vector<agent::ToolSpec> out;
    for (const Spec* spec : registry_.specs()) out.push_back(*spec);
    return out;
}

std::expected<std::unique_ptr<agent::PreparedTool>, agent::ToolResult> ToolSession::prepare(
    std::string_view name, std::string_view arguments) const {
    const Tool* tool = registry_.find(name);
    if (tool == nullptr) return std::unexpected(detail::error_result(std::format("unknown tool: {}", name)));
    return tool->prepare(arguments, context_);
}

void add_mcp(Registry& registry, std::shared_ptr<mcp::Client> client) {
    for (const mcp::Tool& tool : client->tools()) registry.add(detail::make_mcp_tool(client, tool));
}

agent::ResourceIntent to_intent(const workspace::Resolved& resolved, agent::Access access) {
    agent::ResourceIntent intent;
    intent.path = resolved.path;
    intent.access = access;
    intent.inside_workspace = resolved.inside_workspace;
    return intent;
}

} // namespace dagent::tools
