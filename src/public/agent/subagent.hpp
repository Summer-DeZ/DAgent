/// @file subagent.hpp
/// @brief 子 Agent 的派发：从父 Setup 与定义派生子的 Setup，以及 task 工具的实现。
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "agent/agent.hpp"
#include "agent/options.hpp"
#include "agent/permission.hpp"
#include "tools/tools.hpp"

namespace dagent::agent {

/// @brief 派生子 Agent 的 Setup。
Setup derive_child_setup(const Setup& parent, const SubagentDef&, const DerivedPermission&,
                         std::string_view parent_session_id,
                         const std::vector<std::string>& parent_tool_names);

/// @brief 创建 task 工具。depth > 0 或定义表为空时返回 nullptr（模型看不到这个工具）。
/// owner 必须比返回的工具活得久（Agent 持有 Registry，Registry 持有工具）。
std::unique_ptr<tools::Tool> make_task_tool(Agent& owner);

} // namespace dagent::agent
