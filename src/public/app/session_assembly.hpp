/// @file session_assembly.hpp
/// @brief 后端装配：从已解析配置创建 SessionInstance（runtime::SessionFactory 实现）。
///
/// 提示词渲染、工具/MCP 环境、记录写入器与写租约都在这里构造；runtime 只看到核心对象。
/// 同会话切模型复用传入 lease，不再次 flock。
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "agent/options.hpp"
#include "agent/skills.hpp"
#include "agent/port_model.hpp"
#include "agent/public_model.hpp"
#include "tools/mcp_hub.hpp"
#include "workspace/context.hpp"
#include "exec/process.hpp"
#include "exec/sandbox.hpp"
#include "runtime/factory.hpp"
#include "storage/storage.hpp"
#include "tools/tools.hpp"
#include "workspace/files.hpp"
#include "workspace/search.hpp"

namespace dagent::app {

/// @brief 按配置名解析模型：公开描述 + 已配置客户端；凭据不进入公开值。
struct ModelSelection {
    agent::PublicModel provider;
    std::shared_ptr<agent::ModelSession> session;
};

class SessionAssembly final : public runtime::SessionFactory {
public:
    /// @brief 整个后端不变的装配值；每个会话的权限/模型/子 Agent 收窄在创建时单独解析。
    struct Options {
        std::shared_ptr<tools::McpHub> hub;
        workspace::Environment environment;
        std::vector<agent::SubagentDef> subagents;
        std::shared_ptr<const agent::SkillCatalog> skills;
        agent::Options agent;               ///< 上下文预算、调用上限、进度间隔
        std::filesystem::path cwd, project_root, control_root;
        std::optional<std::filesystem::path> git_root;
        tools::Options tools;
        workspace::FileOptions files;
        workspace::SearchOptions search;
        exec::Options process;
        exec::SandboxOptions sandbox_options;
        exec::Support sandbox; ///< 启动时探测一次
        std::optional<exec::SrtRuntime> srt; ///< SRT 后端资源；为空表示受限执行不可用
        storage::Options storage;
        std::string user_instructions;
        std::string system_prompt, compact_prompt; ///< 主会话提示词模板
        runtime::SessionState initial;             ///< 启动权限档/只读/规划；model 空表示 default_model
        ModelSelection default_model;
        std::function<ModelSelection(const std::string&)> resolve_model;
    };

    explicit SessionAssembly(Options options);
    ~SessionAssembly() override;
    SessionAssembly(const SessionAssembly&) = delete;
    SessionAssembly& operator=(const SessionAssembly&) = delete;

    std::string resolve_session(std::optional<std::string_view> prefix) override;
    const agent::SubagentDef* find_subagent(std::string_view name) const override;
    std::unique_ptr<runtime::SessionInstance> create_new(std::optional<runtime::SessionState> state) override;
    std::unique_ptr<runtime::SessionInstance> resume(std::string_view session_id,
                                                    std::optional<runtime::SessionState> state,
                                                    const agent::Sink& replay) override;
    std::unique_ptr<runtime::SessionInstance> prepare_switch_model(std::string_view model_name,
                                                                  std::shared_ptr<agent::SessionLease> lease,
                                                                  const runtime::SessionState& state,
                                                                  const agent::Sink& replay) override;
    std::unique_ptr<runtime::SessionInstance> create_child(const agent::DelegationContext& context,
                                                          const agent::SubagentDef& def,
                                                          const agent::DerivedPermission& permission) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dagent::app
