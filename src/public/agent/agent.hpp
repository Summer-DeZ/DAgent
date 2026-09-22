/// @file agent.hpp
/// @brief 兼容装配点：run 模式（headless）经 Agent 使用 Session/TurnRunner 与子执行。
///
/// 核心循环、提交与调度都在 agent 核心对象里；Agent 只把 Setup 交给 app::SessionAssembly
/// 构造会话实例并转发旧接口。TUI 已在 R07 改用 runtime/Runtime；R10 后 headless 走协议，
/// R13 删除本兼容层。
#pragma once

#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "agent/events.hpp"
#include "agent/permission.hpp"
#include "agent/run.hpp"
#include "agent/run_services.hpp"
#include "agent/session_meta.hpp"
#include "agent/setup.hpp"

namespace dagent::app {
class SessionAssembly;
} // namespace dagent::app

namespace dagent::runtime {
class SessionInstance;
class SubagentExecutor;
} // namespace dagent::runtime

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
    TurnStatus run_turn(std::string input, const TurnContext&);

    /// @brief 手动摘要；不追加用户消息或 turn_end，通过返回值报告完成状态。
    TurnStatus compact(const TurnContext&);

    /// @brief 权限模式（交互界面的 Shift+Tab）。线程安全，下一次决策生效。
    void set_permission_mode(PermissionMode mode);
    void set_read_only(bool value);
    bool read_only() const;
    void set_plan_mode(bool value);
    bool planning() const;
    PermissionMode permission_mode() const;
    std::vector<Policy::SessionGrant> session_grants() const;
    bool revoke_permission(std::string_view id);

    const SessionMeta& meta() const;
    std::vector<std::string> tool_names() const;

private:
    Agent(Setup setup, std::unique_ptr<app::SessionAssembly> factory,
          std::unique_ptr<runtime::SessionInstance> instance);

    std::unique_ptr<app::SessionAssembly> factory_; ///< 子执行的会话工厂
    std::unique_ptr<runtime::SubagentExecutor> subagent_;
    std::unique_ptr<runtime::SessionInstance> instance_;
    std::uint64_t run_seq_ = 0;
};

} // namespace dagent::agent
