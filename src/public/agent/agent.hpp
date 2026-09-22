/// @file agent.hpp
/// @brief 一个会话的全部状态加 `run_turn`：模型 → 工具 → 回填，直到结束。
///
/// Agent 不是线程安全的：所有方法都在调用它的 agent 线程上跑（docs/design/agent.md §1）。
#pragma once

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "agent/conversation.hpp"
#include "agent/compaction.hpp"
#include "agent/events.hpp"
#include "agent/reply.hpp"
#include "agent/mcp_hub.hpp"
#include "agent/options.hpp"
#include "agent/permission.hpp"
#include "agent/record.hpp"
#include "session/session.hpp"
#include "tools/tools.hpp"

namespace dagent::agent {

class Agent {
public:
    /// @brief 新会话：渲染 system prompt、创建会话记录、注册内置工具。失败时抛外围异常。
    static std::unique_ptr<Agent> create(Setup setup);

    /// @brief 恢复会话：回放历史、闭合崩溃中的一轮，再按当前环境重新渲染 system prompt。
    static std::unique_ptr<Agent> resume(Setup setup, std::string_view session_id,
                                         const Sink& replay_sink);

    /// @brief 子 Agent：复用 host 的 Environment 渲染 prompt（不跑 git）、共享 Hub 只取快照、
    /// Recorder 的 Meta 带 parent_id / agent_name、按 allowed_tools 收窄工具集。
    static std::unique_ptr<Agent> create_child(Setup setup);

    ~Agent();
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    /// @brief 一轮。阻塞到结束；除编程错误外不抛异常。
    TurnStatus run_turn(std::string input, const TurnContext&);

    /// @brief 手动摘要；不追加用户消息或 turn_end，通过返回值报告完成状态。
    TurnStatus compact(const TurnContext&);

    /// @brief 权限模式（交互界面的 Shift+Tab）。线程安全，下一次决策生效；调用方保证对象仍存活。
    void set_permission_mode(PermissionMode mode);
    void set_read_only(bool value);
    bool read_only() const;
    void set_plan_mode(bool value);
    bool planning() const;
    PermissionMode permission_mode() const { return policy_.mode(); }
    std::vector<Policy::SessionGrant> session_grants() const;
    bool revoke_permission(std::string_view id);

    /// @brief MCP 连接状态快照。线程安全，调用方保证 Agent 仍存活。
    std::vector<ServerState> mcp_states() const { return hub_->states(); }

    const session::Meta& meta() const { return recorder_.meta(); }

    const TurnContext* current_turn() const { return turn_; }
    const Setup& setup() const { return setup_; }
    std::vector<std::string> tool_names() const;

private:
    struct DispatchOutcome {
        enum class Stop { none, interrupted, denied } stop = Stop::none;
        int handled = 0;        ///< 这一批里计入 max_tool_calls 的调用数
        bool hit_limit = false; ///< 有调用因为超额拿到了 T8
    };

    Agent(Setup setup, std::string system_prompt, Recorder recorder,
          Conversation conversation = {});

    std::vector<ToolDef> tool_defs() const;
    ModelParams model_params() const; ///< 中立请求参数，来自当前 provider 公开值
    DispatchOutcome dispatch(const std::vector<ToolCall>& calls, int budget, const TurnContext&);
    TurnStatus finish(TurnStatus, std::string error, int steps, int calls, const Usage& total,
                      const Sink&);
    void keep_partial(const Reply&, const Sink&);
    void check_broken(const Sink&);
    void report_stream(const StreamEvent&, const Sink&);
    void report_retry(const RetryInfo&, const Sink&);

    // 成员声明顺序即构造顺序，析构倒序：registry_ 先于任何持有 Client 的部件析构（docs/design/agent.md §1）。
    Setup setup_;
    Recorder recorder_;
    std::shared_ptr<McpHub> hub_; ///< registry_ 先析构，确保 MCP 工具不再引用 Client
    tools::Registry registry_;
    tools::Context tool_ctx_;
    Policy policy_;
    std::shared_ptr<ModelSession> model_; ///< 已配置的模型客户端（llm 实现，经端口调用）
    Conversation conversation_;
    TokenEstimator estimator_;
    Compactor compactor_;
    std::string system_prompt_;
    const TurnContext* turn_ = nullptr; ///< 本轮的外部接口，供 task 工具取用；finish() 里清空
    bool broken_notified_ = false;
    int questions_this_turn_ = 0;
};

} // namespace dagent::agent
