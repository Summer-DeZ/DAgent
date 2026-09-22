/// @file options.hpp
/// @brief 核心的 Options 与 Setup：app 把配置和命令行映射成它们，Agent 只认它们、不读配置文件。
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/port_model.hpp"
#include "agent/permission.hpp"
#include "agent/public_model.hpp"
#include "exec/process.hpp"
#include "exec/sandbox.hpp"
#include "mcp/client.hpp"
#include "session/session.hpp"
#include "tools/tools.hpp"
#include "workspace/files.hpp"
#include "workspace/search.hpp"

namespace dagent::agent {

class AgentHost; ///< options.hpp 不能包含 host.hpp（host.hpp 要用 SubagentDef）；shared_ptr 允许不完整类型

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
    int max_parallel_tasks = 4; ///< 并发子 Agent 上限，下限 3；每个都要打模型请求，故低于只读组的 8
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

/// @brief 子 Agent 定义（安装根 home/agents/<name>.md：frontmatter + 正文 system prompt）。
struct SubagentDef {
    std::string name;               ///< task 工具 agent 参数的取值，必须唯一
    std::string description;        ///< 给主模型选择用的说明，写进 task 工具描述
    std::string model;              ///< 空 = 继承父 provider；非空时必须是 models.json 里的名字
    std::vector<std::string> tools; ///< 空 = 父工具集减 task/ask/exit_plan（默认不含 MCP 工具）
    std::string permission = "inherit"; ///< inherit / read_only / ask
    int max_model_calls = 0;        ///< 0 = 继承全局 run.max_model_calls
    int max_tool_calls = 0;         ///< 0 = 继承全局 run.max_tool_calls
    std::string system_prompt;      ///< frontmatter 之后的正文
};

/// @brief 一个 Agent 需要的全部输入（docs/design/agent.md §12）。
struct Setup {
    Options options;

    // 模型：公开描述与已配置的客户端；密钥与 HTTP 细节留在 llm/app 装配侧
    PublicModel provider; ///< 公开模型描述（名字、模型 ID、窗口与输出预算、温度）
    std::shared_ptr<ModelSession> model_session; ///< 已配置的模型客户端；寿命由装配层与 Session 共同保证

    // 工作区
    std::filesystem::path cwd;          ///< 工作区根（Args::cwd）
    std::filesystem::path project_root; ///< Config::project_root
    std::filesystem::path control_root; ///< 配置、提示词、会话与日志所在目录
    std::optional<std::filesystem::path> git_root;

    // 外围
    tools::Options tools;
    workspace::FileOptions files;
    workspace::SearchOptions search;
    exec::Options process;
    exec::SandboxOptions sandbox_options;
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

    // 子 Agent
    std::shared_ptr<AgentHost> host;        ///< 共享运行时；入口创建一次，子 Agent 继承同一个
    std::vector<SubagentDef> subagents;     ///< 可派发的定义；子 Agent 恒为空
    int subagent_depth = 0;                 ///< 0 = 主 Agent；>0 不注册 task 工具
    std::string parent_session_id;          ///< 子会话记录的 parent_id；顶层为空
    std::string subagent_name;              ///< 子会话记录的 agent_name；顶层为空
    std::vector<std::string> allowed_tools; ///< 子 Agent 的工具收窄列表；主 Agent 为空
};

} // namespace dagent::agent
