/// @file prompt.hpp
/// @brief 提示词：内置文本（编进二进制）、开发期覆盖、会话开始时渲染 system prompt。
///
/// 每轮动态修改会破坏前缀缓存（03-conversation §4），所以只在会话开始时渲染一次。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "workspace/context.hpp"

namespace dagent::agent {

/// @brief system.md 的模板变量（08-prompt §3）。
struct PromptVars {
    std::string model;
    std::filesystem::path project_root;
    bool sandbox = false;             ///< exec::probe() 结果：沙箱可用
    std::string permission_mode;      ///< "ask" / "accept_edits" / "auto" / "deny"
};

std::string_view builtin_system_prompt();  ///< prompts/system.md，构建时编入
std::string_view builtin_compact_prompt(); ///< prompts/compact.md，构建时编入

/// @brief 渲染 system prompt；模板出错抛 workspace::bad_template（带行号）。
std::string render_system_prompt(std::string_view tmpl, const workspace::Environment&,
                                 const PromptVars&);

} // namespace dagent::agent
