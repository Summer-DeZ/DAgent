/// @file decision_systemone.hpp
/// @brief System One 协议适配器：编码 state + questions，解码概率，复用 net::HttpClient。
///
/// 只编码协议与解码响应；问句由核心提供，不在此生成或改写。失败统一返回 DecisionError，
/// 不静默重试；排队与 HTTP 等待都计入 deadline 与 stop。
#pragma once

#include <chrono>
#include <memory>

#include "agent/port_decision.hpp"
#include "llm/decision_provider.hpp"
#include "net/http.hpp"

namespace dagent::llm {

/// @brief 创建 System One 决策模型客户端。
/// 同一实例内部串行使用一个 HttpClient（服务只有单个处理槽）；
/// 调用方可从多个线程使用该端口，实际 HTTP 传输始终只有一个使用者。
std::shared_ptr<agent::DecisionModel> make_systemone_decision_model(
    DecisionProviderConfig config, std::chrono::milliseconds timeout, net::HttpOptions http);

} // namespace dagent::llm
