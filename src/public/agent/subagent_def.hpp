/// @file subagent_def.hpp
/// @brief 子 Agent 定义（安装根 home/agents/<name>.md：frontmatter + 正文 system prompt）。
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace dagent::agent {

struct SubagentDef {
    std::string name;               ///< task 工具 agent 参数的取值，必须唯一
    std::string description;        ///< 给主模型选择用的说明，写进 task 工具描述
    std::string model;              ///< 空 = 继承父 provider；非空时必须是 models.json 里的名字
    std::optional<std::vector<std::string>> tools; ///< 未指定则继承父工具集并排除控制/MCP；显式空列表禁用工具
    std::string permission = "inherit"; ///< inherit / read_only / ask
    int max_model_calls = 0;        ///< 0 = 继承全局 run.max_model_calls
    int max_tool_calls = 0;         ///< 0 = 继承全局 run.max_tool_calls
    std::string system_prompt;      ///< frontmatter 之后的正文
};

} // namespace dagent::agent
