/// @file config.hpp
/// @brief 安装目录中的 config.json + models.json 配置加载。
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "agent/options.hpp"
#include "base/log.hpp"
#include "exec/process.hpp"
#include "lib/nlohmann/json.hpp"
#include "mcp/client.hpp"
#include "net/http.hpp"
#include "session/session.hpp"
#include "tools/tools.hpp"
#include "workspace/files.hpp"
#include "workspace/search.hpp"

namespace dagent::app {

struct Config {
    struct Ui { std::filesystem::path theme_file; } ui;
    std::map<std::string, agent::ProviderConfig> models;
    std::string model;
    std::vector<std::string> model_selection_log; ///< 日志初始化后输出 CLI 选择/覆盖路径
    std::filesystem::path system_prompt_file;
    std::filesystem::path compact_prompt_file;
    net::HttpOptions http;
    exec::Options process;
    exec::SandboxOptions sandbox;
    workspace::FileOptions files;
    workspace::SearchOptions search;
    session::Options session;
    base::LogOptions log;
    mcp::Options mcp;
    tools::Options tools;
    agent::Options agent; ///< context / run / progress / permissions
    std::map<std::string, std::string> credentials;
    std::vector<mcp::ServerConfig> mcp_servers;
    std::vector<agent::SubagentDef> subagents; ///< <root>/agents/*.md
    std::filesystem::path root;
    std::filesystem::path project_root;
};

struct InstallationPaths {
    std::filesystem::path root;
    std::filesystem::path config;
    std::filesystem::path models;
    std::filesystem::path database;
    std::filesystem::path logs;
};

struct LoadOptions {
    std::filesystem::path root;
    std::filesystem::path cwd;
    std::vector<std::string> overrides;
};

class ConfigError : public std::runtime_error {
public:
    enum class Kind {
        parse,   ///< JSON 语法错误（含注释语法）、顶层不是对象
        type,    ///< 类型不符，信息里带 JSON 指针路径
        io,      ///< 文件不存在或读不了
        invalid, ///< 值不合法：${VAR} 引用的变量不存在、MCP server 名冲突
    };

    ConfigError(Kind kind, const std::string& what) : std::runtime_error(what), kind_(kind) {}
    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

/// @brief DAGENT_HOME、dev 默认 home 或 /proc/self/exe 的父目录；同时验证根存在、是目录且可写。
InstallationPaths installation_paths();

/// @brief 读取安装目录中的两份 JSON，再叠加 overrides。未知键只记 warn。
Config load_config(const LoadOptions&);

/// @brief 读取 <root>/agents/*.md：frontmatter + 正文。目录不存在时返回空，不报错。
std::vector<agent::SubagentDef> load_subagents(const std::filesystem::path& dir,
                                               const std::map<std::string, agent::ProviderConfig>& models);

/// @brief 校验并追加一个模型配置，原子重写 models.json（保持 0600）；重名时拒绝。
/// 返回值中的 env:KEY 已解析，供当前进程立即切换使用。
agent::ProviderConfig add_model(const std::filesystem::path& root,
                                const agent::ProviderConfig& model);

/// @brief git 根（`git rev-parse --show-toplevel`），不在仓库里或 git 不可用时退回 cwd。
std::filesystem::path project_root(const std::filesystem::path& cwd);

/// @brief 把 `{"mcpServers": {...}}` 映射成 mcp::ServerConfig；${VAR} 从进程环境展开，变量不存在时抛
/// invalid（指出 server 和字段）。server 名按 mcp::sanitize_name 清理后必须互不相同、且不含 "__"。
/// `type` 为 "sse" 或无法识别的 server 只记 warn 并跳过。
std::vector<mcp::ServerConfig> parse_mcp_servers(const nlohmann::json& root);

} // namespace dagent::app
