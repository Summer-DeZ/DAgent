/// @file runtime.hpp
/// @brief Runtime：backend 协议适配看到的会话控制外观。
///
/// 持有串行会话调度、交互代理与子执行；前端只提交意图、
/// 消费快照/事件并回答交互，不持具体装配对象或写业务状态。
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "agent/events.hpp"
#include "agent/model_input.hpp"
#include "agent/public_model.hpp"
#include "runtime/state.hpp"
#include <condition_variable>
#include <deque>
#include <expected>
#include <mutex>
#include <thread>
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
    bool start(const StartOptions& options);
    RuntimeSnapshot snapshot() const;


    using CommandDone = std::function<void(std::expected<void, RuntimeError>)>;
    using GrantRevoked = std::function<void(std::expected<bool, RuntimeError>)>;
    using ModelAdded = std::function<void(agent::PublicModel, std::string selection_error)>;

    std::expected<std::string, RuntimeError> submit(std::string text,
                                                    std::function<void(const std::string&)> accepted = {});
    std::optional<QueuedInput> recall_last();
    std::expected<void, RuntimeError> new_session(CommandDone done = {});
    std::expected<void, RuntimeError> resume(std::string session_id, CommandDone done = {});
    std::expected<void, RuntimeError> select_model(std::string name, CommandDone done = {});
    std::expected<void, RuntimeError> add_model(agent::ModelInput input, ModelAdded done = {});
    std::expected<void, RuntimeError> compact();
    std::expected<void, RuntimeError> revoke_grant(std::string grant_id, GrantRevoked done);
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
    std::vector<agent::McpServerState> mcp_states() const;
    struct RunControl {
        std::stop_source stop;
    };
    struct CurrentRun {
        std::string id;
        std::string operation;
        std::shared_ptr<RunControl> control;
    };
    enum class State { empty, ready, executing, replacing, closing, closed };
    struct Command {
        enum class Kind { new_session, resume, select_model, add_model, compact, revoke_grant };
        Kind kind = Kind::new_session;
        std::string value;
        agent::ModelInput model;
        CommandDone done;
        ModelAdded model_done;
        GrantRevoked grant_done;
    };
    struct PendingReplace {
        std::unique_ptr<SessionInstance> instance;
        std::vector<agent::Event> replay;
        bool resumed = false;
    };

    void worker(std::stop_token stop);
    void execute_command(const Command& command);
    bool front_ready_locked() const { return !queue_.empty() && queue_.front().ready; }
    void drain();
    void run_input(const QueuedInput& input);
    void run_compact();

    std::expected<void, RuntimeError> enqueue(Command command);
    PendingReplace prepare_new();
    PendingReplace prepare_resume(const std::string& session_id);
    PendingReplace prepare_switch(const std::string& model_name);
    void install(PendingReplace pending, bool replace_transcript);

    void publish(EventPayload payload, const std::string& session_id, std::uint64_t generation);
    void publish_control(ControlEvent event);
    void sync_control_snapshot();
    void finalize_execution();
    void refresh_snapshot(SessionInstance& instance);
    void apply_to_snapshot(const Event& event);
    std::string next_input_id();
    std::string next_run_id();

    mutable std::mutex mutex_;                  ///< 控制状态：实例/队列/命令/状态
    std::condition_variable_any cv_;
    struct QueueEntry {
        QueuedInput input;
        bool ready = true; ///< 接受响应已入发送队列后才允许出队
    };
    std::deque<Command> commands_;
    bool command_pending_ = false; ///< 命令从接受到执行结束均占用空闲入口
    std::deque<QueueEntry> queue_;
    std::shared_ptr<SessionInstance> current_;
    std::uint64_t generation_ = 0;
    State state_ = State::empty;
    std::optional<CurrentRun> run_;
    std::uint64_t input_seq_ = 0;
    std::uint64_t run_seq_ = 0;
    bool closed_ = false;

    mutable std::mutex publish_mutex_; ///< 快照更新与事件发布
    RuntimeSnapshot snapshot_;

    void interaction_requested(const InteractionRequest& request) override;
    void interaction_closed(const std::string& interaction_id) override;

    std::shared_ptr<ConfigurationGateway> configuration_;
    std::unique_ptr<SessionFactory> factory_;
    InteractionBroker broker_;
    std::unique_ptr<SubagentExecutor> subagent_;
    bool interactive_;
    Frontend* frontend_ = nullptr;
    std::jthread worker_;
};

} // namespace dagent::runtime
