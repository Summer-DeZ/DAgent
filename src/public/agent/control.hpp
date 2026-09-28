/// @file control.hpp
/// @brief 控制动作：ask/exit_plan/todo/task 的类型化请求、固定 Schema 与执行器。
///
/// 控制动作的解析结果进入 ControlRequest，
/// 由 ControlActionExecutor 按各自规则处理。
#pragma once

#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agent/action.hpp"
#include "agent/events.hpp"
#include "agent/permission.hpp"
#include "agent/run.hpp"
#include "agent/subagent_def.hpp"
#include "agent/work_plan.hpp"

namespace dagent::agent {

class DelegationChannel; ///< port_delegation.hpp

/// @brief ask：向用户提问。
struct AskRequest {
    std::string call_id;
    std::string summary;
    AskView view;
};

/// @brief exit_plan：计划确认。
struct PlanConfirmation {
    std::string call_id;
    std::string summary;
    AskView view;
    std::string plan; ///< 模型提交的完整方案
};

/// @brief todo：整份计划替换。
struct PlanReplacement {
    std::string summary;
    TodoView plan;
};

/// @brief task：一次父子委派。
struct DelegationRequest {
    std::string call_id;
    std::string summary;
    std::string agent, prompt;
};

struct SkillActivation {
    std::string summary, name;
};

using ControlRequest = std::variant<AskRequest, PlanConfirmation, PlanReplacement, DelegationRequest, SkillActivation>;

/// @brief 准备结果：普通工具或类型化控制请求。
using PreparedAction = std::variant<std::unique_ptr<PreparedTool>, ControlRequest>;

/// @brief 控制动作的固定描述，顺序为 todo、ask、exit_plan、task（与原注册顺序一致）。
std::vector<ToolSpec> control_action_specs(bool include_task, const std::vector<SubagentDef>& subagents);

/// @brief 解析一个控制动作调用；错误返回 is_error 的 ToolResult（保留原错误文本）。
std::expected<ControlRequest, ToolResult> parse_control_action(std::string_view name,
                                                               std::string_view arguments,
                                                               std::string_view call_id,
                                                               const std::vector<SubagentDef>& subagents);

/// @brief 控制动作执行器：ask 计数、计划选项语义、todo 计划替换事实与 task 委派。
class ControlActionExecutor {
public:
    /// @brief 本轮可用的能力；由 Session 按当前 Run 在开始执行前绑定，寿命覆盖整轮。
    struct Services {
        const Asker* asker = nullptr;      ///< 空 = 非交互：ask/exit_plan 按原结果返回，不等待
        const Approver* approver = nullptr; ///< 委派上下文里的父审批入口；空表示不可审批
        DelegationChannel* delegation = nullptr;
        Policy* policy = nullptr;
        bool base_read_only = false;       ///< exit_plan 接受后恢复的启动只读初值
        Sink sink;
        Run* run = nullptr;              ///< owner 线程中的当前 Run；只在阻塞问答前后更新阶段
        std::string session_id;          ///< 委派上下文的父会话身份
        std::string model;               ///< 父当前模型配置名（默认子模型继承）
        std::vector<std::string> tool_names; ///< 委派默认子名单
        std::function<ToolResult(std::string_view)> activate_skill;
    };

    ControlActionExecutor() = default;

    /// @brief 绑定本轮能力并清零本轮问答计数。
    void begin_turn(Services services);

    /// @brief 执行一个控制请求（ask/exit_plan 可能阻塞等待问答）；返回工具结果。
    /// todo 只返回计划替换事实，WorkPlan 由 SessionCommitter 在原序提交点更新。
    ToolResult execute(const ControlRequest&, std::stop_token);

private:
    enum class PlanDecision { accept_workspace, accept_ask, continue_planning };

    ToolResult run_ask(const AskRequest&, std::stop_token);
    ToolResult run_plan_confirmation(const PlanConfirmation&, std::stop_token);
    ToolResult run_plan_replacement(const PlanReplacement&);
    ToolResult run_delegation(const DelegationRequest&, std::stop_token);
    PlanDecision plan_decision(const Answer&) const;

    Services services_;
    int questions_this_turn_ = 0;
};

} // namespace dagent::agent
