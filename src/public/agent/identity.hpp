/// @file identity.hpp
/// @brief 核心身份值类型：会话、轮次、调用与输入的标识。
///
/// 这些身份只在当前后端实例内有效；session_id 是唯一的持久身份。
#pragma once

#include <string>

namespace dagent::agent {

/// 持久会话身份（SessionMeta::id，UUIDv7）。
using SessionId = std::string;

/// 一次 turn / compact 的运行身份；后端实例前缀加计数，不落库。
using RunId = std::string;

/// 一次 Run 内的动作/步骤身份。
using InvocationId = std::string;

/// 后端内存队列里的一条输入身份；用于原子取回。
using InputId = std::string;

/// 一次待回答交互的身份；Run 结束后不能复用。
using InteractionId = std::string;

} // namespace dagent::agent
