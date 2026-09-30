/// @file prompt.hpp
/// @brief 提示词：从安装根读取模板，会话开始时渲染 system prompt。
///
/// 每轮动态修改会破坏前缀缓存（docs/design/agent.md §4），所以只在会话开始时渲染一次。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/context.hpp"

namespace dagent::app {

/// @brief system.md 的模板变量（docs/design/agent.md §9）。
struct PromptVars {
    std::string model;
    std::filesystem::path project_root;
    bool sandbox = false;             ///< 受限执行探测结果：沙箱可用
    bool workspace_sandbox = false;
    std::string sandbox_backend;
    std::vector<std::string> sandbox_missing;
    bool sandbox_child_signals = false;  ///< 沙箱内命令能否向自己的子进程发信号
    std::string permission_mode;      ///< "ask" / "workspace" / "unrestricted"
};

/// @brief 渲染 system prompt；模板出错抛 workspace::bad_template（带行号）。
std::string render_system_prompt(std::string_view tmpl, const workspace::Environment&,
                                 const PromptVars&);

} // namespace dagent::app
