/// @file port_lease.hpp
/// @brief SessionLease 端口：一个可写会话的跨进程所有权句柄。
///
/// storage 的 flock 实现是唯一具体实现；runtime 只持有寿命与身份，不依赖 storage
/// （architecture-refactor §4.3/§9）。同一进程内同会话复用同一个句柄。
#pragma once

#include <string>

namespace dagent::agent {

class SessionLease {
public:
    virtual ~SessionLease() = default;

    /// @brief 该租约保护的持久会话 ID。
    virtual const std::string& session_id() const = 0;
};

} // namespace dagent::agent
