#include "agent/action.hpp"

#include <exception>
#include <format>

#include "base/log.hpp"

namespace dagent::agent {

std::optional<ToolResult> PreparedTool::prepare_preview(const ExecutionGrant& grant) {
    if (grant.authority_stop.stop_requested()) {
        ToolResult result;
        result.is_error = true;
        result.model_text = "Parent permission authority was revoked before preview; prepare again.";
        return result;
    }
    try { return do_prepare_preview(grant); }
    catch (const std::exception& e) {
        ToolResult result;
        result.is_error = true;
        result.model_text = std::format("preview failed: {}", e.what());
        return result;
    }
}

// execute 不抛异常是有意的：核心对每个调用只需要处理一种返回值。取消返回 interrupted；环境问题
// （rg 没装、沙箱准备失败、MCP 断连）模型修不了，但也应该知道，作为 is_error 的结果返回并记日志。
ToolResult PreparedTool::execute(const ExecutionGrant& grant,
                                 const std::function<void(std::string_view)>& on_output,
                                 std::stop_token stop) {
    if (stop.stop_requested() || grant.authority_stop.stop_requested()) {
        ToolResult result;
        result.interrupted = true;
        result.model_text = "Execution cancelled before consuming its permission grant.";
        return result;
    }
    try {
        return do_execute(grant, on_output, std::move(stop));
    } catch (const std::exception& e) {
        base::logger("tools")->warn("tool call failed: {}", e.what());
        ToolResult result;
        result.model_text = std::format("tool failed: {}", e.what());
        result.is_error = true;
        return result;
    }
}

} // namespace dagent::agent
