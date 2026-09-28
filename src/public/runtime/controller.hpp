/// @file controller.hpp
/// @brief SessionController：当前顶层会话、普通输入队列、一个当前操作与 generation。
///
/// 所有可写操作经同一个串行执行线程决定先后；队列出队与新建/恢复/切模型/压缩都在这里，
/// UI 只提交意图并消费快照与事件（状态机 §1/§2）。取消/回答不经此队列。
#pragma once

#include <cstddef>
#include <filesystem>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "agent/mcp_state.hpp"
#include "agent/model_input.hpp"
#include "agent/permission.hpp"
#include "agent/port_delegation.hpp"
#include "agent/public_model.hpp"
#include "agent/work_plan.hpp"
#include "runtime/factory.hpp"
#include "runtime/interaction.hpp"

namespace dagent::runtime {

/// @brief 一条仍在后端内存队列里的普通输入。
struct QueuedInput {
    std::string id;
    std::string text;
};

/// @brief 面向前端的只读状态；取值与事件投影在同一短锁边界发布。
struct RuntimeSnapshot {
    std::string session_id;
    std::uint64_t generation = 0;
    agent::PublicModel model;
    agent::PermissionMode permission_mode = agent::PermissionMode::workspace;
    bool planning = false;
    bool read_only = false;
    bool busy = false;
    std::string operation; ///< "turn" / "compact" / "replacing"；空表示空闲
    std::string run_id;
    std::vector<QueuedInput> queue;
    std::size_t used_tokens = 0;
    std::size_t token_limit = 0;
    std::size_t window_tokens = 0;
    int trigger_percent = 80;
    std::filesystem::path cwd, project_root;
    agent::TodoView work_plan;
    std::vector<agent::McpServerState> mcp;
    bool recording_broken = false;
    std::string recording_error;
    std::vector<agent::Policy::SessionGrant> grants;
};

/// @brief runtime 层的控制事件：不属于核心执行事实的状态变化与操作结果。
struct ControlEvent {
    enum class Kind {
        session_replaced,   ///< new/resume/切模型安装完成（前端重置或刷新历史）
        operation_finished, ///< 手动压缩结束（清 busy 并 drain）
        failed,             ///< 控制操作失败；operation 指出动作
    };
    Kind kind = Kind::session_replaced;
    std::string operation;
    std::string error;
    agent::TurnStatus status = agent::TurnStatus::done;
    bool replace_transcript = true; ///< session_replaced 时前端是否重置 Transcript（切模型为 false）
    bool resumed = false;           ///< session_replaced 来自显式恢复（前端显示恢复头并读取历史）
};

using EventPayload = std::variant<agent::Event, ControlEvent>;

struct Event {
    std::string session_id;
    std::uint64_t generation = 0;
    EventPayload payload;
};

/// @brief 发布出口；必须在锁内快速入队，不得阻塞（背压由具体实现自己处理）。
using EventSink = std::function<void(const Event&)>;

struct RuntimeError {
    enum class Kind {
        busy,
        stale_session,
        not_found,
        session_in_use,
        invalid_state,
        config_error,
        query_failed,
        startup_failed,
        closing,
        interaction_closed,
    };
    Kind kind = Kind::invalid_state;
    std::string message;
};

struct StartOptions {
    std::optional<std::string> resume_id;
    bool continue_last = false;
};

class SessionController {
public:
    struct Deps {
        SessionFactory* factory = nullptr; ///< 由 Runtime 持有，寿命覆盖控制器
        ConfigurationGateway* configuration = nullptr;
        InteractionBroker* broker = nullptr;
        agent::DelegationChannel* delegation = nullptr; ///< 子执行适配；可为空（非交互装配）
        EventSink sink;
    };

    explicit SessionController(Deps deps);
    ~SessionController();
    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;

    /// @brief 同步创建/恢复初始会话；失败抛异常，由启动装配处理。
    bool start(const StartOptions&);
    /// @brief 关闭：停止新输入、清队列、取消当前 Run、唤醒交互、join 执行线程。
    void shutdown();

    RuntimeSnapshot snapshot() const;
    /// @brief 当前 MCP 连接状态快照（线程安全）。
    std::vector<agent::McpServerState> mcp_states() const;

    /// @brief 加入普通输入队列；accepted 在入队后、Run 可开始前调用（必须快速入队响应，不得回调本对象）。
    std::expected<std::string, RuntimeError> submit(std::string text,
                                                    std::function<void(const std::string&)> accepted = {});
    std::optional<QueuedInput> recall_last();

    /// new/resume/select/add 的异步结果回调（执行线程调用）；为空时只用事件通知。
    using CommandDone = std::function<void(std::expected<void, RuntimeError>)>;
    using GrantRevoked = std::function<void(std::expected<bool, RuntimeError>)>;
    using ModelAdded = std::function<void(agent::PublicModel, std::string selection_error)>;

    std::expected<void, RuntimeError> new_session(CommandDone done = {});
    std::expected<void, RuntimeError> resume(std::string session_id, CommandDone done = {});
    std::expected<void, RuntimeError> select_model(std::string name, CommandDone done = {});
    std::expected<void, RuntimeError> add_model(agent::ModelInput input, ModelAdded done = {});
    std::expected<void, RuntimeError> compact();
    std::expected<void, RuntimeError> revoke_grant(std::string grant_id, GrantRevoked done);

    std::expected<void, RuntimeError> cycle_permission();
    std::expected<void, RuntimeError> toggle_planning();
    /// @brief 只取消身份仍匹配的当前 Run；旧 run_id 返回 false。
    bool cancel(std::string_view run_id);
    /// @brief 解析 --resume/--continue 目标为完整 session id。
    std::expected<std::string, RuntimeError> resolve_session(std::optional<std::string_view> prefix);

private:
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

    Deps deps_;
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

    std::jthread worker_;
};

} // namespace dagent::runtime
