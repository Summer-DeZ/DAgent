/// @file identity.hpp
/// @brief 核心身份值类型：会话与轮次的标识。
///
/// 这些身份只在当前后端实例内有效；session_id 是唯一的持久身份。
#pragma once

#include <string>

namespace dagent::agent {

/// 持久会话身份（SessionMeta::id，UUIDv7）。
using SessionId = std::string;

/// 一次 turn / compact 的运行身份；后端实例前缀加计数，不落库。
using RunId = std::string;

} // namespace dagent::agent
