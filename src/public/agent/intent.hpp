/// @file intent.hpp
/// @brief 权限决策所需的中立意图摘要：资源访问、命令影响与交互内容。
///
/// 核心（Policy、调度器）只看这些值；exec 分析树、MCP Client、workspace 实现对象
/// 留在 tools 模块的 PreparedTool 实现内部（architecture-refactor §4.2）。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "agent/tool_data.hpp"

namespace dagent::agent {

/// 工具调用的类别；与既有权限/调度语义一一对应。
enum class ToolKind { read, write, exec, external, ask, exit_plan, task };

/// 资源访问方向。
enum class Access { read, write };

/// @brief 一次路径访问的意图：规范化路径、方向与工作区内外关系。
struct ResourceIntent {
    std::filesystem::path path; ///< 绝对路径，已规范化并解析符号链接
    Access access = Access::read;
    bool inside_workspace = true;
};

/// bash 语法状态；由原 exec 分析得出，核心不重写解析。
enum class SyntaxState { valid, error, incomplete };

/// 命令影响的类别。
enum class ImpactKind { read, write, network, special };

/// @brief 一条已解析/不确定的影响；target 为空表示运行时才知道。
struct CommandImpact {
    ImpactKind kind = ImpactKind::special;
    std::string target, reason;
    bool dynamic = false;
};

/// @brief 一条 bash 命令的中立摘要。dangerous / known_readonly 由原 exec 分析函数在
/// tools 侧计算成布尔值，不能在核心重写简化白名单（architecture-refactor §4.2）。
struct CommandIntent {
    std::string command;      ///< 原始命令
    int analysis_version = 0; ///< 分析器版本
    SyntaxState syntax = SyntaxState::valid;
    std::string syntax_message; ///< 语法状态非 valid 时的说明
    bool dynamic = false;       ///< 存在运行时展开、控制流或未知程序
    bool known_readonly = false;
    bool dangerous = false;
    bool cwd_unknown = false;
    std::vector<CommandImpact> impacts; ///< 已解析路径/网络影响和不确定影响
};

/// @brief 一次已准备调用的完整意图摘要：权限决策、调度分组与审批展示的全部输入。
struct PreparedIntent {
    ToolKind kind = ToolKind::read;
    std::vector<ResourceIntent> paths;  ///< read/write 涉及的路径
    std::optional<CommandIntent> command; ///< exec 时有效
    std::string preview;                ///< write/edit：unified diff，给确认对话框
    std::string summary;                ///< 一行描述，如「编辑 src/a.cpp（+3 −1）」
    AskView ask;                        ///< ask / exit_plan 的交互内容
    std::string plan_summary;           ///< exit_plan：模型提交的方案
};

} // namespace dagent::agent
