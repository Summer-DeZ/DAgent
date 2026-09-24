/// @file runtime.hpp
/// @brief Runtime：backend 协议适配看到的会话控制外观。
///
/// 组装 SessionController、交互代理与子执行；前端只提交意图、
/// 消费快照/事件并回答交互，不持具体装配对象或写业务状态。
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "agent/events.hpp"
#include "agent/model_input.hpp"
#include "agent/public_model.hpp"
#include "runtime/controller.hpp"
#include "runtime/factory.hpp"
#include "runtime/interaction.hpp"

namespace dagent::runtime {

class SubagentExecutor;

/// @brief 前端事件出口。方法从后端线程调用，实现负责转到自己的线程；不得阻塞。
class Frontend {
public:
    virtual ~Frontend() = default;

    virtual void event(const Event&) = 0;
    virtual void interaction_requested(const InteractionRequest&) = 0;
    virtual void interaction_closed(const std::string& interaction_id) = 0;
};

class Runtime final : public InteractionOutlet {
public:
    struct Deps {
        std::unique_ptr<SessionFactory> factory;
        std::shared_ptr<ConfigurationGateway> configuration;
        Frontend* frontend = nullptr;
        bool interactive = true; ///< false 时审批/问答按非交互结果返回，不生成永远等不到的对话框
    };

    explicit Runtime(Deps deps);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// @brief 同步创建/恢复初始会话；失败抛 std::exception（由启动装配处理）。
    StartResult start(const StartOptions& options);
    RuntimeSnapshot snapshot() const;


    using CommandDone = SessionController::CommandDone;
    using ModelAdded = SessionController::ModelAdded;

    std::expected<std::string, RuntimeError> submit(std::string text,
                                                    std::function<void(const std::string&)> accepted = {});
    std::optional<QueuedInput> recall_last();
    std::expected<void, RuntimeError> new_session(CommandDone done = {});
    std::expected<void, RuntimeError> resume(std::string session_id, CommandDone done = {});
    std::expected<void, RuntimeError> select_model(std::string name, CommandDone done = {});
    std::expected<void, RuntimeError> add_model(agent::ModelInput input, ModelAdded done = {});
    std::expected<void, RuntimeError> compact();
    std::expected<bool, RuntimeError> revoke_grant(const std::string& grant_id);
    std::expected<void, RuntimeError> cycle_permission();
    std::expected<void, RuntimeError> toggle_planning();
    bool cancel(std::string_view run_id);
    std::expected<std::string, RuntimeError> resolve_session(std::optional<std::string_view> prefix);

    /// @brief 回答一个待处理交互；已关闭返回 false（interaction_closed）。
    bool answer(const std::string& interaction_id, agent::Decision decision);
    bool answer(const std::string& interaction_id, agent::Answer answer);

    /// @brief 关闭后端：停止输入、取消执行、唤醒交互、join 执行线程。
    void shutdown();

private:
    void interaction_requested(const InteractionRequest& request) override;
    void interaction_closed(const std::string& interaction_id) override;

    std::shared_ptr<ConfigurationGateway> configuration_;
    std::unique_ptr<SessionFactory> factory_;
    InteractionBroker broker_;
    std::unique_ptr<SubagentExecutor> subagent_;
    std::unique_ptr<SessionController> controller_;
    Frontend* frontend_ = nullptr;
};

} // namespace dagent::runtime
