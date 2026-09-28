/// @file identity.hpp
/// @brief 核心身份值类型：持久会话标识。
///
/// 运行控制身份由 runtime 持有，不进入核心执行状态。
#pragma once

#include <string>

namespace dagent::agent {

/// 持久会话身份（SessionMeta::id，UUIDv7）。
using SessionId = std::string;

} // namespace dagent::agent
