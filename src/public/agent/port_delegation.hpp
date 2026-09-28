/// @file port_delegation.hpp
/// @brief DelegationChannel 端口：一次 task 委派的执行入口与父侧只读上下文。
///
/// 核心只传已解析的请求与执行时取样的父事实；子会话构造、模型/工具名单与生命周期
/// 由 runtime/SubagentExecutor 负责。
/// 不含父 Agent&、current_turn 指针或可写父 Session。
#pragma once

#include <stop_token>
#include <string>
#include <vector>

#include "agent/control.hpp"
#include "agent/events.hpp"
#include "agent/permission.hpp"
#include "agent/tool_data.hpp"

namespace dagent::agent {

/// @brief 一次委派的父侧不可变上下文；执行时构造，寿命覆盖本次调用。
struct DelegationContext {
    std::string parent_session_id;
    std::string call_id;  ///< 父会话里这次 task 调用的 model_call_id
    PermissionMode parent_mode = PermissionMode::workspace;
    bool parent_planning = false;
    bool parent_read_only = false;
    std::string model;                     ///< 父当前模型配置名；子定义未指定模型时继承
    std::vector<std::string> parent_tools; ///< 父当前工具名单（派生默认子名单）
    const Sink* sink = nullptr;            ///< 父事件出口；子事件以 SubEvent 包装后送往这里
    const Approver* approver = nullptr;    ///< 父审批入口；空表示不可审批
    std::stop_token stop;
};

class DelegationChannel {
public:
    virtual ~DelegationChannel() = default;

    /// @brief 阻塞执行一次委派；返回原 task 工具结果（含 TaskView）。
    virtual ToolResult delegate(const DelegationContext&, const DelegationRequest&) = 0;
};

} // namespace dagent::agent
