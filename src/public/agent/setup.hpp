/// @file setup.hpp
/// @brief 兼容装配输入；具体工具、存储与执行配置不进入 agent 核心。
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/options.hpp"
#include "agent/port_model.hpp"
#include "agent/public_model.hpp"
#include "agent/subagent_def.hpp"
#include "exec/process.hpp"
#include "exec/sandbox.hpp"
#include "mcp/client.hpp"
#include "storage/storage.hpp"
#include "tools/tools.hpp"
#include "workspace/files.hpp"
#include "workspace/search.hpp"

namespace dagent::app {
class Assembly;
} // namespace dagent::app

namespace dagent::agent {

/// @brief 一个会话装配需要的全部启动输入（兼容结构；最终由 app 装配持有，R13 删除）。
struct Setup {
    Options options;

    PublicModel provider;
    std::shared_ptr<ModelSession> model_session;

    std::filesystem::path cwd, project_root, control_root;
    std::optional<std::filesystem::path> git_root;

    tools::Options tools;
    workspace::FileOptions files;
    workspace::SearchOptions search;
    exec::Options process;
    exec::SandboxOptions sandbox_options;
    storage::Options session;
    mcp::Options mcp;
    std::vector<mcp::ServerConfig> mcp_servers;

    exec::Support sandbox;
    PermissionMode permission_mode = PermissionMode::workspace;
    bool read_only = false;
    bool planning = false;
    std::string system_prompt, compact_prompt;

    std::shared_ptr<app::Assembly> assembly;
    std::vector<SubagentDef> subagents;
    int subagent_depth = 0;
    std::string parent_session_id, subagent_name;
    std::vector<std::string> allowed_tools;
};

} // namespace dagent::agent
