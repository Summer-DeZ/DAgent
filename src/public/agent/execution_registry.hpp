/// @file execution_registry.hpp
/// @brief 活跃执行登记：权限变化时重新核对并即时终止不再被当前策略覆盖的运行实例。
///
/// 权限事实仍然只在 Policy；登记表只保存执行开始时的授权副本与自己的取消入口，
/// 供 runtime 在撤销/降权时请求停止，不构成第二个授权来源。
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "agent/grant.hpp"
#include "agent/intent.hpp"
#include "agent/message.hpp"

namespace dagent::agent {

class Policy;

/// @brief 一条正在运行的普通工具执行。stop 请求会终止对应进程/连接。
class ActiveExecution {
public:
    ActiveExecution(const ActiveExecution&) = delete;
    ActiveExecution& operator=(const ActiveExecution&) = delete;

    /// @brief 记录一次已放行的运行中网络目标；persisted 表示批准来自配置或会话规则。
    void note_network(const NetworkTarget& target, bool persisted);
    /// @brief 权限核对写入的终止原因；空表示未被权限变化终止。
    std::string termination_reason() const;

    std::stop_source stop;
    ToolCall call;
    PreparedIntent intent;
    ExecutionGrant grant;

private:
    friend class ExecutionRegistry;

    ActiveExecution() = default;

    mutable std::mutex mutex_;
    std::vector<std::pair<NetworkTarget, bool>> networks_;
    std::string termination_reason_;
    std::unique_ptr<std::stop_callback<std::function<void()>>> relay_;
};

/// @brief 一个会话的活跃执行集合；线程安全。
class ExecutionRegistry {
public:
    /// @brief 注册一次执行；run_stop 触发时同步终止该执行。
    std::shared_ptr<ActiveExecution> begin(const ToolCall& call, const PreparedIntent& intent,
                                           const ExecutionGrant& grant, std::stop_token run_stop);
    void end(const std::shared_ptr<ActiveExecution>& execution);

    /// @brief 权限发生变化后核对所有活跃执行：不再被当前策略允许、使用了已撤销网络目标，
    /// 或授权范围被收窄的执行会被请求停止。返回被终止执行的 call_id 与原因。
    std::vector<std::pair<std::string, std::string>> reconcile(const Policy& policy);

private:
    std::mutex mutex_;
    std::vector<std::shared_ptr<ActiveExecution>> active_;
};

} // namespace dagent::agent
