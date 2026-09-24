/// @file port_interaction.hpp
/// @brief InteractionChannel 端口：审批与问答的唯一交互入口。
///
/// 实现在 runtime；核心控制动作/授权流程只请求交互，不关心前端形态。
/// 等待期间不持有任何业务锁；取消唤醒等待并返回现有取消语义。
#pragma once

#include <stop_token>

#include "agent/events.hpp"

namespace dagent::agent {

class InteractionChannel {
public:
    virtual ~InteractionChannel() = default;

    /// @brief 请求一次审批；返回用户的决定。无交互能力或取消时返回现有非交互/取消结果。
    virtual Decision request_approval(const Approval&, std::stop_token) = 0;

    /// @brief 请求一次问答；返回用户的回答（含 cancelled）。
    virtual Answer request_answer(const Question&, std::stop_token) = 0;
};

} // namespace dagent::agent
