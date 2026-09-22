/// @file runtime.hpp
/// @brief Runtime：前端（进程内或经协议）看到的后端外观。
///
/// 组装 SessionController、交互代理、子执行与只读查询；前端只提交意图、
/// 消费快照/事件并回答交互，不持 Agent/Setup 或写业务状态（R07 完成条件）。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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
        std::shared_ptr<QueryGateway> queries;
        Frontend* frontend = nullptr;
        bool interactive = true; ///< false 时审批/问答按非交互结果返回，不生成永远等不到的对话框
    };

    explicit Runtime(Deps deps);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// @brief 同步创建/恢复初始会话；失败抛 std::exception（由启动装配处理）。
    StartResult start(const StartOptions& options);
    /// @brief 绑定前端出口；在 start 之前调用。
    void set_frontend(Frontend* frontend);
    RuntimeSnapshot snapshot() const;

    std::vector<agent::PublicModel> models() const;
    std::vector<agent::ProviderKindInfo> provider_kinds() const;
    std::filesystem::path theme_file() const;

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
    void cancel();
    bool cancel(std::string_view run_id);
    std::expected<std::string, RuntimeError> resolve_session(std::optional<std::string_view> prefix);

    /// @brief 回答一个待处理交互；已关闭返回 false（interaction_closed）。
    bool answer(const std::string& interaction_id, agent::Decision decision);
    bool answer(const std::string& interaction_id, agent::Answer answer);

    // 只读查询：在查询线程执行，结果经回调返回（回调在查询线程上调用）。
    using SessionsResult = std::expected<std::vector<SessionSummary>, RuntimeError>;
    using HistoryResult = std::expected<std::vector<agent::Event>, RuntimeError>;
    using WorkspaceResult = std::expected<WorkspaceInfo, RuntimeError>;
    using FilesResult = std::expected<std::vector<FileCandidate>, RuntimeError>;
    void query_sessions(std::size_t limit, std::function<void(SessionsResult)> done);
    void query_history(std::string session_id, std::function<void(HistoryResult)> done);
    void query_workspace(std::function<void(WorkspaceResult)> done);
    void query_files(std::string query, std::size_t limit, std::function<void(FilesResult)> done);

    /// @brief 关闭后端：停止输入、取消执行、唤醒交互、join 查询与执行线程。
    void shutdown();

private:
    void interaction_requested(const InteractionRequest& request) override;
    void interaction_closed(const std::string& interaction_id) override;
    void post_query(std::function<void()> job);
    void query_worker(std::stop_token stop);

    std::shared_ptr<ConfigurationGateway> configuration_;
    std::shared_ptr<QueryGateway> queries_;
    std::unique_ptr<SessionFactory> factory_;
    InteractionBroker broker_;
    std::unique_ptr<SubagentExecutor> subagent_;
    std::unique_ptr<SessionController> controller_;
    std::atomic<Frontend*> frontend_{nullptr};

    std::mutex query_mutex_;
    std::condition_variable_any query_cv_;
    std::deque<std::function<void()>> query_jobs_;
    bool query_closed_ = false;
    std::jthread query_thread_;
};

} // namespace dagent::runtime
