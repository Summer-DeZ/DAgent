/// @file snapshot_projection.hpp
/// @brief Runtime 内部快照投影；不获取 Runtime 锁、不发布事件、不读取调度状态。
#pragma once

#include "runtime/state.hpp"

namespace dagent::runtime {

class SessionInstance;

namespace detail {

/// 在允许读取会话完整状态的线程上调用；控制字段由 Runtime 随后补齐。
RuntimeSnapshot snapshot_from_session(SessionInstance&);

/// 调用方持有 publish_mutex_；只按事件语义更新相关字段。
void apply_snapshot_event(RuntimeSnapshot&, const Event&);

} // namespace detail
} // namespace dagent::runtime
