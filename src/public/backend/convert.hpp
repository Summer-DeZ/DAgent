/// @file convert.hpp
/// @brief backend 适配：核心/runtime 值 → 协议 DTO 的唯一转换点。
#pragma once

#include <string>
#include <utility>

#include "agent/history.hpp"
#include "lib/nlohmann/json.hpp"
#include "protocol/dto.hpp"
#include "protocol/rpc.hpp"
#include "runtime/controller.hpp"

namespace dagent::backend {

protocol::PublicModel to_protocol(const agent::PublicModel& model);
protocol::SessionSnapshot to_protocol(const runtime::RuntimeSnapshot& snapshot);
protocol::HistoryItem to_protocol(const agent::HistoryItem& item);

/// @brief 核心事件 → (kind, data)；子事件在 backend 展开身份。
std::pair<std::string, nlohmann::json> split_event(const agent::Event& event);

/// @brief runtime 业务错误 → 统一 RPC error（code=-32000 + data.kind）。
protocol::RpcError to_rpc_error(const runtime::RuntimeError& error);

} // namespace dagent::backend
