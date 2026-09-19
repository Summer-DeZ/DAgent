/// @file agent.hpp
/// @brief 一个会话的全部状态加 `run_turn`：模型 → 工具 → 回填，直到结束。
///
/// Agent 不是线程安全的：所有方法都在调用它的 agent 线程上跑（README §5）。
#pragma once

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "agent/conversation.hpp"
#include "agent/events.hpp"
#include "agent/model.hpp"
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

    ~Agent();
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    /// @brief 一轮。阻塞到结束；除编程错误外不抛异常。
    TurnStatus run_turn(std::string input, const Sink& sink, const Approver& approver,
                        std::stop_token stop);

    /// @brief 权限模式（交互界面的 Shift+Tab）。线程安全，下一次决策生效；调用方保证对象仍存活。
    void set_permission_mode(PermissionMode mode);

    const session::Meta& meta() const { return recorder_.meta(); }

private:
    struct DispatchOutcome {
        enum class Stop { none, interrupted, denied } stop = Stop::none;
        int handled = 0;        ///< 这一批里计入 max_tool_calls 的调用数
        bool hit_limit = false; ///< 有调用因为超额拿到了 T8
    };

    Agent(Setup setup, std::string system_prompt, Recorder recorder,
          Conversation conversation = {});

    std::vector<ToolDef> tool_defs() const;
    DispatchOutcome dispatch(const std::vector<ToolCall>& calls, int budget, const Sink&,
                             const Approver&, std::stop_token);
    TurnStatus finish(TurnStatus, std::string error, int steps, int calls, const Usage& total,
                      const Sink&);
    void keep_partial(const Reply&, const Sink&);
    void check_broken(const Sink&);
    void report_stream(const StreamEvent&, const Sink&);
    void report_retry(const RetryInfo&, const Sink&);

    // 成员声明顺序即构造顺序，析构倒序：registry_ 先于任何持有 Client 的部件析构（04-turn §3）。
    Setup setup_;
    Recorder recorder_;
    tools::Registry registry_;
    tools::Context tool_ctx_;
    Policy policy_;
    Model model_;
    Conversation conversation_;
    TokenEstimator estimator_;
    std::string system_prompt_;
    bool broken_notified_ = false;
};

} // namespace dagent::agent
