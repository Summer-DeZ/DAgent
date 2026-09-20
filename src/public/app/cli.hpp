/// @file cli.hpp
/// @brief 命令行解析：子命令、通用选项与 stdin 提示词的合并。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dagent::app {

/// trust：把 project_root(cwd) 加入信任列表（由入口调用 app::trust_project 完成）。
enum class Mode { interactive, run, sessions, trust, models };
enum class OutputFormat { text, json, jsonl };

struct Args {
    Mode mode = Mode::interactive;
    std::string prompt; ///< 多个词用空格拼接；run 模式已经合并了 stdin 的内容
    std::filesystem::path cwd; ///< 工作区根：-C 指定的目录或启动目录（trust 模式下是要信任的目录）。
                               ///< 不会 chdir，所有模块的路径都要基于它传入
    std::optional<std::filesystem::path> config_file;
    std::vector<std::string> overrides; ///< -m 按 argv 顺序保存为内部 @model=… 标记
    std::optional<std::string> resume_id;
    bool continue_last = false;
    std::optional<std::string> log_level;
    std::optional<std::string> permissions; ///< --permissions 显式给了才有值；否则用配置（docs/design/agent.md §7）
    OutputFormat output = OutputFormat::text;
};

/// @brief --help、--version 和解析错误会直接打印并返回退出码；其他情况返回 Args。
std::variant<Args, int> parse_args(int argc, char** argv);

} // namespace dagent::app
