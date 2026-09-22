/// @file factory.hpp
/// @brief runtime 的外部装配端口：会话工厂、配置网关与只读查询网关。
///
/// 具体实现在 app 后端装配（R09 的 dagent_app_config）；runtime 只经这些端口访问
/// storage/llm/tools/workspace 等具体实现（architecture-refactor §4.3）。
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/events.hpp"
#include "agent/history.hpp"
#include "agent/mcp_state.hpp"
#include "agent/model_input.hpp"
#include "agent/permission.hpp"
#include "agent/port_lease.hpp"
#include "agent/port_delegation.hpp"
#include "agent/public_model.hpp"
#include "agent/run_services.hpp"
#include "agent/session.hpp"
#include "agent/subagent_def.hpp"

namespace dagent::runtime {

/// @brief 一个已装配会话：核心 Session 与其执行环境（工具目录、MCP 资源、记录写入）同寿。
///
/// runtime 只看到核心类型；具体 environment（tools::Context/Registry/JournalWriter）在 app 装配内。
class SessionInstance {
public:
    virtual ~SessionInstance() = default;

    virtual agent::Session& session() = 0;
    /// @brief 本会话的 MCP 步骤边界与断连文本出口（SessionResources 端口）。
    virtual agent::SessionResources& resources() = 0;
    /// @brief 当前 MCP 连接状态快照（线程安全）。
    virtual std::vector<agent::McpServerState> mcp_states() const = 0;
    /// @brief 本会话持有的可写所有权；同会话切模型时转交候选，寿命与实例相同。
    virtual std::shared_ptr<agent::SessionLease> lease() const = 0;
};

/// @brief 会话控制事实：新会话/恢复/模型替换要保留或重置的状态（B07/B13/B14）。
struct SessionState {
    agent::PermissionMode mode = agent::PermissionMode::workspace;
    bool planning = false;
    bool read_only = false; ///< 启动只读初值（Policy 的 base）
    std::string model;      ///< 当前模型配置名；空表示装配初值
};

/// @brief 从已解析配置创建会话；候选准备失败抛异常，当前会话不受影响。
class SessionFactory {
public:
    virtual ~SessionFactory() = default;

    /// @brief 解析 --resume/--continue 目标为完整 session id；prefix 为空表示最近会话。
    virtual std::string resolve_session(std::optional<std::string_view> prefix) = 0;
    /// @brief 子 Agent 定义表查询（装配配置）。
    virtual const agent::SubagentDef* find_subagent(std::string_view name) const = 0;

    /// @brief 新建顶层会话（L01）；permission 为空时用装配初值（启动创建）。
    virtual std::unique_ptr<SessionInstance> create_new(std::optional<SessionState> state,
                                                        const agent::Sink& replay) = 0;
    /// @brief 显式恢复（L20）：候选自己取得写租约；失败时旧会话保持。permission 为空时用装配初值。
    virtual std::unique_ptr<SessionInstance> resume(std::string_view session_id,
                                                    std::optional<SessionState> state,
                                                    const agent::Sink& replay) = 0;
    /// @brief 同会话切模型候选（L21）：复用当前 lease，按 B13 重置临时授权/FileTracker/token 校准。
    virtual std::unique_ptr<SessionInstance> prepare_switch_model(std::string_view model_name,
                                                                  std::shared_ptr<agent::SessionLease> lease,
                                                                  const SessionState& state,
                                                                  const agent::Sink& replay) = 0;
    /// @brief 子会话（L02）：从父上下文与子定义派生；子自己取得写租约。
    virtual std::unique_ptr<SessionInstance> create_child(const agent::DelegationContext& context,
                                                          const agent::SubagentDef& def,
                                                          const agent::DerivedPermission& permission,
                                                          const agent::Sink& replay) = 0;
};

/// @brief 后端配置操作：模型清单/添加与主题文件路径；凭据只在此边界转换，不进入公开返回值。
class ConfigurationGateway {
public:
    virtual ~ConfigurationGateway() = default;

    virtual std::vector<agent::PublicModel> models() const = 0;
    virtual std::vector<agent::ProviderKindInfo> provider_kinds() const = 0;
    /// @brief 校验并原子保存模型；失败抛异常（前端按 config_error 处理）。返回保存后的公开描述。
    virtual agent::PublicModel add_model(const agent::ModelInput&) = 0;
    virtual std::filesystem::path theme_file() const = 0;
};

struct SessionSummary {
    std::string id;
    std::string title;
    std::chrono::system_clock::time_point updated;
};

struct WorkspaceInfo {
    std::filesystem::path cwd;
    std::filesystem::path project_root;
    std::string branch;
};

struct FileCandidate {
    std::string path;
    bool directory = false;
};

struct ChildSummary {
    std::string session_id, agent, title;
};

/// @brief 一页只读历史；cursor 由后端原样回传（记录路线 §7）。
struct HistoryPage {
    std::vector<agent::HistoryItem> items;
    std::string cursor;
    bool done = false;
};

/// @brief 一个打开的历史查询：固定高水位、跨页验证游标；释放后读取返回错误。
class HistoryReader {
public:
    virtual ~HistoryReader() = default;

    virtual HistoryPage read(const std::string& cursor, std::size_t limit) = 0;
    virtual const std::string& session_id() const = 0;
    virtual std::int64_t upper_seq() const = 0;
};

/// @brief 只读查询：列表、历史投影、项目信息与文件补全；在查询线程调用，不碰当前 Writer。
class QueryGateway {
public:
    virtual ~QueryGateway() = default;

    virtual std::vector<SessionSummary> sessions(std::size_t limit) = 0;
    virtual std::vector<agent::Event> history(std::string_view session_id) = 0;
    virtual std::unique_ptr<HistoryReader> open_history(std::string_view session_id) = 0;
    virtual std::vector<ChildSummary> children(std::string_view session_id) = 0;
    virtual WorkspaceInfo workspace() = 0;
    virtual std::vector<FileCandidate> complete(std::string_view query, std::size_t limit) = 0;
};

} // namespace dagent::runtime
