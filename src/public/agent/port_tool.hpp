/// @file port_tool.hpp
/// @brief ToolSession 端口：普通工具的稳定描述与准备入口。
///
/// tools 实现（Registry + 会话 Context）；核心 ActionCatalog/Session 只经此访问，
/// 看不到工作区、FileTracker 或 MCP Client 实现（architecture-refactor §4.3）。
#pragma once

#include <expected>
#include <memory>
#include <string_view>
#include <vector>

#include "agent/action.hpp"
#include "agent/message.hpp"
#include "agent/tool_data.hpp"

namespace dagent::agent {

class ToolSession {
public:
    virtual ~ToolSession() = default;

    /// @brief 按注册顺序返回普通工具描述（不含控制动作）。
    virtual std::vector<ToolSpec> specs() const = 0;

    /// @brief 按名字准备普通工具；参数/环境错误返回 is_error 结果。
    virtual std::expected<std::unique_ptr<PreparedTool>, ToolResult> prepare(
        std::string_view name, std::string_view arguments, const InvocationContext&) const = 0;
};

} // namespace dagent::agent
