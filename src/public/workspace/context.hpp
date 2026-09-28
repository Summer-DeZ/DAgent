/// @file context.hpp
/// @brief 项目上下文：环境事实（cwd/OS/shell/日期）、git 信息、逐级向上的 AGENTS.md，以及模板渲染。
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>
#include "exec/process.hpp"

#include "lib/nlohmann/json.hpp"
#include "workspace/error.hpp"

namespace dagent::workspace {

struct GitStatus {
    bool dirty = false;
    std::string branch;                      ///< detached HEAD 时是短 SHA
    std::string status_summary;              ///< "3 个文件已修改，1 个未跟踪"
};

struct GitInfo {
    std::filesystem::path root;
    GitStatus status;
    std::vector<std::string> recent_commits; ///< git log --oneline -n 5
};

struct Instructions {
    std::filesystem::path file;
    std::string content;
    bool truncated = false;
};

struct Environment {
    std::filesystem::path cwd;
    std::string os, shell, date;
    std::optional<GitInfo> git;
    std::vector<Instructions> instructions; ///< 由外到内：用户全局 → 仓库根 → … → cwd
};

struct ContextOptions {
    exec::Options process;
    std::chrono::milliseconds git_timeout{2000};
    std::size_t max_instructions_bytes = 32 << 10;
    std::vector<std::string> instruction_files{"AGENTS.md"};
};

/// @brief 仅查询分支与工作区变更，不读取提交日志或项目指令。
std::optional<GitStatus> collect_git_status(const std::filesystem::path& cwd,
                                           const ContextOptions& = {}, std::stop_token = {});

/// @brief 收集环境事实。git 不可用（没装、不是仓库、超时）时 git 为空，不影响启动。
Environment collect_environment(const std::filesystem::path& cwd, const ContextOptions& = {},
                                std::stop_token = {});

/// @brief 转成模板变量：cwd、os、shell、date、git、instructions。
nlohmann::json to_json(const Environment&);

/// @brief 用 inja 渲染模板；出错时抛 bad_template，信息里带行号。
std::string render(std::string_view tmpl, const nlohmann::json& data);

} // namespace dagent::workspace
