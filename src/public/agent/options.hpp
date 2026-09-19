/// @file options.hpp
/// @brief 核心的 Options 与 Setup：app 把配置和命令行映射成它们，Agent 只认它们、不读配置文件。
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "agent/model.hpp"
#include "agent/openai_chat.hpp"
#include "agent/permission.hpp"
#include "exec/process.hpp"
#include "exec/sandbox.hpp"
#include "mcp/client.hpp"
#include "net/http.hpp"
#include "session/session.hpp"
#include "tools/tools.hpp"
#include "workspace/files.hpp"
#include "workspace/search.hpp"

namespace dagent::agent {

/// @brief 上下文预算，对应 config "context" 段（07-context §2）。
struct ContextOptions {
    std::size_t window_tokens = 262144;
    std::size_t safety_margin_tokens = 8192;
    int compaction_trigger_percent = 80;
    int compaction_target_percent = 60;
};

/// @brief 一轮运行的调用上限，对应 "run" 段。
struct Limits {
    int max_model_calls = 24;
    int max_tool_calls = 35;
    int max_model_retries = 2;
};

/// @brief 进度提示间隔，对应 "progress" 段。
struct ProgressOptions {
    std::chrono::milliseconds interval{1000}; ///< run 模式心跳、界面计时刷新
};

struct Options {
    ContextOptions context;
    Limits run;
    ProgressOptions progress;
    PermissionMode permissions = PermissionMode::automatic; ///< 只作用于 run 模式（06-permission §7）
};

/// @brief 一个 Agent 需要的全部输入（11-entry §2）。
struct Setup {
    Options options;

    // 模型
    ModelParams model;
    OpenAiChatOptions codec; ///< 含 api_key
    net::HttpOptions http;   ///< 已按 02-model §6 调整：timeout = 0

    // 工作区
    std::filesystem::path cwd;          ///< 工作区根（Args::cwd）
    std::filesystem::path project_root; ///< Config::project_root
    std::optional<std::filesystem::path> git_root;

    // 外围
    tools::Options tools;
    workspace::FileOptions files;
    workspace::SearchOptions search;
    exec::Options process;
    session::Options session;
    mcp::Options mcp;
    std::vector<mcp::ServerConfig> mcp_servers;

    // 运行环境
    exec::Support sandbox;                              ///< exec::probe()，启动时探测一次
    PermissionMode permission_mode = PermissionMode::ask; ///< 交互：ask；run：--permissions 或配置
    std::optional<std::string> system_prompt_override;  ///< gateway.system_prompt_file 的内容，已读好
};

} // namespace dagent::agent
