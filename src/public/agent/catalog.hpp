/// @file catalog.hpp
/// @brief ActionCatalog：模型侧动作的稳定顺序描述与准备入口。
///
/// 普通工具来自 ToolSession；控制动作由核心解析。顺序固定为内置普通工具、控制动作、
/// 动态 MCP 工具。
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/control.hpp"
#include "agent/message.hpp"
#include "agent/port_tool.hpp"
#include "agent/subagent_def.hpp"

namespace dagent::agent {

class ActionCatalog {
public:
    struct Config {
        std::vector<std::string> allowed_tools; ///< 空 = 全部；子 Agent 的收窄名单
        std::vector<SubagentDef> subagents;     ///< task 的描述与枚举
        std::shared_ptr<const SkillCatalog> skills;
        bool include_task = false;              ///< 主 Agent 且定义表非空
    };

    ActionCatalog(ToolSession& tools, Config config);

    /// @brief 固定顺序：read…glob、todo、ask/exit_plan、task，动态工具稳定追加。
    std::vector<ToolSpec> specs() const;
    std::vector<std::string> names() const;
    bool contains(std::string_view name) const;

    /// @brief 按名准备普通工具或解析控制动作；未知名字/参数错误返回 is_error 结果。
    std::expected<PreparedAction, ToolResult> prepare(std::string_view name, std::string_view arguments,
                                                      const InvocationContext&) const;

private:
    bool control_enabled(std::string_view name) const;

    ToolSession& tools_;
    Config config_;
    std::vector<ToolSpec> control_specs_;
};

} // namespace dagent::agent
