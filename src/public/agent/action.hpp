/// @file action.hpp
/// @brief 动作契约：一次已准备普通工具的只读意图与执行入口。
///
/// 参数在 prepare 构造时固定；execute 只接收决定好的授权、输出接收器和取消。
/// shell 分析树、MCP Client、workspace 实现对象留在具体 PreparedTool 内部。
#pragma once

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include "agent/grant.hpp"
#include "agent/intent.hpp"
#include "agent/tool_data.hpp"

namespace dagent::agent {

/// @brief 准备完成的普通工具：构造时拥有参数/分析，公开只读 intent。
class PreparedTool {
public:
    virtual ~PreparedTool() = default;
    PreparedTool(const PreparedTool&) = delete;
    PreparedTool& operator=(const PreparedTool&) = delete;

    const PreparedIntent& intent() const { return intent_; }

    virtual std::optional<PreparedIntent> preview_request() const { return std::nullopt; }
    std::optional<ToolResult> prepare_preview(const ExecutionGrant&);

    /// @brief 在调用线程上阻塞执行；不抛异常：取消返回 interrupted，环境失败返回 is_error 结果。
    ToolResult execute(const ExecutionGrant& grant,
                       const std::function<void(std::string_view)>& on_output, std::stop_token stop);

protected:
    PreparedTool() = default;

    PreparedIntent intent_; ///< 由各实现的 prepare 填好（中立摘要）

private:
    virtual std::optional<ToolResult> do_prepare_preview(const ExecutionGrant&) { return std::nullopt; }
    virtual ToolResult do_execute(const ExecutionGrant&, const std::function<void(std::string_view)>&,
                                  std::stop_token) = 0;

};

} // namespace dagent::agent
