/// @file run_services.hpp
/// @brief 一轮运行所需的外部能力与共享资源；不含配置、终端、数据库路径或查找任意对象的方法。
#pragma once

#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>

#include "agent/events.hpp"
#include "agent/port_delegation.hpp"

namespace dagent::agent {

/// @brief 资源侧错误：MCP 等待/刷新被取消或失败；核心不引用具体 MCP 类型。
class ResourceError : public std::runtime_error {
public:
    enum class Kind { cancelled, failed };

    ResourceError(Kind kind, const std::string& what) : std::runtime_error(what), kind_(kind) {}
    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

/// @brief 父子共享的环境事实与 MCP 生命周期；主会话在步骤边界刷新，子会话只用创建时快照。
class SessionResources {
public:
    virtual ~SessionResources() = default;

    /// @brief 主会话模型步骤边界：交付待发通知、等待/重连/合并 MCP 工具。取消抛 ResourceError::cancelled。
    virtual void begin_step(const Sink&, std::stop_token) = 0;
    /// @brief 执行信号：返回追加给模型的说明（T12/T13），不是新断开时返回空。
    virtual std::string mark_disconnected(std::string_view server, std::string_view reason) = 0;
    /// @brief 一轮结束交付尚未报告的 MCP 通知。
    virtual void report_pending(const Sink&) = 0;
};

/// @brief 一轮运行的外部接口；引用寿命覆盖整个 Run。
struct RunServices {
    const Sink& sink;
    const Approver& approver;
    const Asker& asker;
    DelegationChannel* delegation = nullptr;
    SessionResources* resources = nullptr;
    std::stop_token stop;
};

} // namespace dagent::agent
