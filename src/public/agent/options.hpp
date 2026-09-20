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
#include "agent/provider.hpp"
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

/// @brief 上下文预算，对应 config "context" 段（docs/design/agent.md §8）。
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
    PermissionMode permissions = PermissionMode::workspace;
    bool read_only = false;
};

/// @brief 一个 Agent 需要的全部输入（docs/design/agent.md §12）。
struct Setup {
    Options options;

    // 模型
    ProviderConfig provider; ///< 含 api_key
    net::HttpOptions http;   ///< 已按 docs/design/agent.md §3 调整：timeout = 0

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
    PermissionMode permission_mode = PermissionMode::workspace; ///< --permissions 或 config.json
    bool read_only = false;
    bool planning = false;
    std::string system_prompt;  ///< home/system.md 或配置指定文件的内容
    std::string compact_prompt; ///< home/compact.md 或配置指定文件的内容
};

} // namespace dagent::agent
