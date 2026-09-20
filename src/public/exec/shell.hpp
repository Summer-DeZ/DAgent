/// @file shell.hpp
/// @brief bash 命令分析：描述语法、命令结构和已知影响；不承担执行隔离。
#pragma once

#include <filesystem>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dagent::exec {

inline constexpr int kShellAnalysisVersion = 2;

struct SourceRange {
    std::size_t begin = 0, end = 0;
};

enum class SyntaxStatus { valid, error, incomplete };

enum class ImpactKind { read, write, network, special };

struct Impact {
    ImpactKind kind = ImpactKind::special;
    std::string target; ///< 可确定的字面路径/目标；为空表示运行时才知道
    std::string reason;
    SourceRange range;
    bool dynamic = false;
};

struct SimpleCommand {
    std::vector<std::string> argv;  ///< 已还原引号与转义的字面量；含动态展开时可能不完整
    std::string source;             ///< 该简单命令的完整原文
    SourceRange range;
    bool arguments_dynamic = false;
};

struct Analysis {
    int version = kShellAnalysisVersion;
    std::string source;
    SyntaxStatus syntax = SyntaxStatus::valid;
    std::optional<SourceRange> syntax_range;
    std::string syntax_message;
    /// 由 && || ; | 连接起来的每一条简单命令；控制流与命令替换里的命令也会收集。
    std::vector<SimpleCommand> commands;
    std::vector<Impact> impacts;
    /// 存在运行时展开、控制流、解释器或未知程序；动态不等于恶意，也不等于语法错误。
    bool dynamic = false;
    /// 某个 cd 目标或分支使后续相对路径基准不能静态确定。
    bool cwd_unknown = false;
};

/// @brief 用 tree-sitter-bash 纯分析一条命令；不会执行替换、脚本或函数。
Analysis analyze(std::string_view bash_source);

/// @brief 所有子命令都可静态认定只读，且没有动态影响。
bool is_known_readonly(const Analysis& analysis);

/// @brief 同上，但允许字面量 `cd` 到 workspace 内；cd 到其它位置仍不是已知只读。
bool is_known_readonly(const Analysis& analysis, const std::filesystem::path& workspace_root);

/// @brief 系统级不可逆命令的短硬拦名单；任何权限模式都不得执行。
bool is_dangerous(const Analysis& analysis);

} // namespace dagent::exec
