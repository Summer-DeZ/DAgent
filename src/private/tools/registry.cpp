#include "tools/tools.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "base/log.hpp"
#include "tools/detail.hpp"

namespace dagent::tools {

// run 不抛异常是有意的：核心对每个调用只需要处理一种返回值。取消返回 interrupted；环境问题
// （rg 没装、沙箱准备失败、MCP 断连）模型修不了，但也应该知道，作为 is_error 的结果返回并记日志。
Result Call::run(const Grant& grant, const std::function<void(std::string_view)>& on_output,
                 std::stop_token stop) {
    try {
        return do_run(grant, on_output, std::move(stop));
    } catch (const std::exception& e) {
        base::logger("tools")->warn("工具调用失败：{}", e.what());
        return detail::error_result(std::format("工具执行失败：{}", e.what()));
    }
}

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
}

void add_mcp(Registry& registry, mcp::Client& client) {
    for (const mcp::Tool& tool : client.tools()) registry.add(detail::make_mcp_tool(client, tool));
}

} // namespace dagent::tools
