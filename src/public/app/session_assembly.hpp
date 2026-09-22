/// @file session_assembly.hpp
/// @brief 后端装配：从已解析配置创建 SessionInstance（runtime::SessionFactory 实现）。
///
/// 提示词渲染、工具/MCP 环境、记录写入器与写租约都在这里构造；runtime 只看到核心对象。
/// 同会话切模型复用传入 lease，不再次 flock（记录路线 §8.2）。
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "agent/model_input.hpp"
#include "agent/port_model.hpp"
#include "agent/public_model.hpp"
#include "agent/setup.hpp"
#include "app/assembly.hpp"
#include "runtime/factory.hpp"

namespace dagent::app {

/// @brief 按配置名解析模型：公开描述 + 已配置客户端；凭据不进入公开值。
struct ModelSelection {
    agent::PublicModel provider;
    std::shared_ptr<agent::ModelSession> session;
};

class SessionAssembly final : public runtime::SessionFactory {
public:
    struct Options {
        std::shared_ptr<Assembly> assembly;
        agent::Setup base; ///< 启动初值：cwd/tools/session/mcp/sandbox/prompts 等
        std::function<ModelSelection(const std::string&)> resolve_model;
    };

    explicit SessionAssembly(Options options);
    ~SessionAssembly() override;
    SessionAssembly(const SessionAssembly&) = delete;
    SessionAssembly& operator=(const SessionAssembly&) = delete;

    std::string resolve_session(std::optional<std::string_view> prefix) override;
    const agent::SubagentDef* find_subagent(std::string_view name) const override;
    std::unique_ptr<runtime::SessionInstance> create_new(std::optional<runtime::SessionState> state,
                                                        const agent::Sink& replay) override;
    std::unique_ptr<runtime::SessionInstance> resume(std::string_view session_id,
                                                    std::optional<runtime::SessionState> state,
                                                    const agent::Sink& replay) override;
    std::unique_ptr<runtime::SessionInstance> prepare_switch_model(std::string_view model_name,
                                                                  std::shared_ptr<agent::SessionLease> lease,
                                                                  const runtime::SessionState& state,
                                                                  const agent::Sink& replay) override;
    std::unique_ptr<runtime::SessionInstance> create_child(const agent::DelegationContext& context,
                                                          const agent::SubagentDef& def,
                                                          const agent::DerivedPermission& permission,
                                                          const agent::Sink& replay) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dagent::app
