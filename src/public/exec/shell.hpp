/// @file shell.hpp
/// @brief bash 命令分析：拆出简单命令、判断能否静态认定为只读。权限策略由核心基于这个结果决定。
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dagent::exec {

struct SimpleCommand {
    std::vector<std::string> argv;  ///< 已还原引号与转义的字面量；含动态展开时可能不完整
};

struct Analysis {
    /// 由 && || ; | 连接起来的每一条简单命令；控制流与命令替换里的命令也会收集。
    std::vector<SimpleCommand> commands;
    /// 含有 $(…)、反引号、变量展开、写文件/heredoc 重定向、eval、控制流、未知语法等
    /// 无法静态判断的结构。为 true 时绝不能自动放行。
    bool has_opaque = false;
};

/// @brief 用 tree-sitter-bash 解析一条命令。解析错误按 has_opaque 处理，不会抛异常。
Analysis analyze(std::string_view bash_source);

/// @brief 所有子命令都在只读白名单里，并且 has_opaque=false。
bool is_known_readonly(const Analysis& analysis);

} // namespace dagent::exec
