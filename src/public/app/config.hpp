/// @file config.hpp
/// @brief 配置加载：按优先级合并多层 JSON，映射成各模块的 Options，并解析密钥。
///
/// **只有这个模块知道配置文件里的键名**；其他模块只认自己的 Options。不做热重载，也不写回配置
/// （唯一会写的是信任列表，见 trust_project）。
///
/// 项目级的 `.dagent/config.json` 与 `.mcp.json` 只在项目受信任时读取：它们能改网关地址、执行命令，
/// 克隆来的仓库不能默认拥有这些能力。项目的 `.env` / `.env.dev` 与 AGENTS.md 不受限制。
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/dotenv.hpp"
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

/// @brief 模型网关，对应 "gateway" 段。api_key 已经从 Secrets 取出，本地网关可以为空。
struct Gateway {
    std::string base_url, model;
    int max_tokens = 4096;
    std::optional<double> temperature;
    bool enable_thinking = false;
    std::filesystem::path system_prompt_file; ///< 已解析成绝对路径
    std::string api_key;
};

/// @brief 上下文预算，对应 "context" 段。核心落地前先在这里定义并校验。
struct ContextOptions {
    int window_tokens = 262144;
    int safety_margin_tokens = 8192;
    int compaction_trigger_percent = 80;
    int compaction_target_percent = 60;
};

/// @brief 一轮运行的调用上限，对应 "run" 段。
struct RunOptions {
    int max_model_calls = 24;
    int max_tool_calls = 20;
    int max_model_retries = 2;
};

/// @brief 进度提示间隔，对应 "progress" 段。
struct ProgressOptions {
    std::chrono::milliseconds interval{1000};
};

struct Config {
    Gateway gateway;
    net::HttpOptions http;
    exec::Options process;
    workspace::FileOptions files;
    workspace::SearchOptions search;
    session::Options session;
    base::LogOptions log;
    mcp::Options mcp;
    tools::Options tools;
    ContextOptions context;
    RunOptions run;
    ProgressOptions progress;
    std::string permissions = "auto";
    std::map<std::string, std::string> credentials; ///< network.credentials：主机名 → 密钥值
    std::vector<mcp::ServerConfig> mcp_servers;      ///< 用户级 mcp.json + 项目级 .mcp.json（受信任时）
    std::vector<std::filesystem::path> sources;      ///< 实际参与合并的 dagent 配置文件，优先级低到高

    std::filesystem::path project_root;                ///< 项目根（git 根或 cwd），信任以它为单位
    bool project_trusted = false;                      ///< 项目根在信任列表里
    std::vector<std::filesystem::path> untrusted_files; ///< 存在但因项目未受信任而没读的项目级文件；
                                                        ///< 非空时交互界面应询问是否信任
};

struct LoadOptions {
    std::filesystem::path cwd;                          ///< 工作目录，默认当前目录
    std::optional<std::filesystem::path> explicit_file; ///< 指定后替换用户级与项目级，不参与合并
    std::vector<std::string> overrides;                 ///< "a.b.c=value"，优先级最高
    std::optional<std::filesystem::path> project_root;  ///< 为空时找 git 根，再退回 cwd
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

/// @brief 合并用户级 `$XDG_CONFIG_HOME/dagent/config.json`、项目级 `<root>/.dagent/config.json`
/// （受信任时）或显式文件，再叠加 overrides，映射成 Config。未知键只记 warn。
Config load_config(const LoadOptions&, const base::Secrets&);

/// @brief git 根（`git rev-parse --show-toplevel`），不在仓库里或 git 不可用时退回 cwd。
std::filesystem::path project_root(const std::filesystem::path& cwd);

/// @brief 信任列表 `$XDG_CONFIG_HOME/dagent/trusted_projects.json` 里有没有这个项目根（按规范化路径
/// 精确比较，不含子目录继承）。列表不存在或损坏时按空处理（损坏记 warn）。
bool is_trusted(const std::filesystem::path& project_root);

/// @brief 把项目根加入信任列表（原子写入，已在列表里则不变）。交互界面确认后、或 `dagent trust` 调用。
void trust_project(const std::filesystem::path& project_root);

/// @brief 密钥来源，同一个键以先读到的为准（进程环境变量始终最优先，见 Secrets::get）：
/// 用户级 `$XDG_CONFIG_HOME/dagent/.env`（权限必须是 0600 或更严，否则跳过并 warn）→ 项目根的
/// `.env.dev` → `.env`。用户级排在项目级前面，不可信仓库的 .env 盖不住用户自己的密钥。
base::Secrets load_secrets(const std::filesystem::path& project_root);

/// @brief 把 `{"mcpServers": {...}}` 映射成 mcp::ServerConfig；${VAR} 用 Secrets 展开，变量不存在时抛
/// invalid（指出 server 和字段）。server 名按 mcp::sanitize_name 清理后必须互不相同、且不含 "__"。
/// `type` 为 "sse" 或无法识别的 server 只记 warn 并跳过。
std::vector<mcp::ServerConfig> parse_mcp_servers(const nlohmann::json& root, const base::Secrets&);

} // namespace dagent::app
