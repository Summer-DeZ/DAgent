/// @file port_model.hpp
/// @brief ModelSession 端口：核心执行对象调用模型的唯一入口。
///
/// 实现在 llm 模块（R03）；HTTP、SSE、凭据与厂商编解码都不进入核心。
/// complete 返回 Reply 或抛现有分类的 ModelError（cancelled/context_too_long/rejected/exhausted）。
#pragma once

#include <functional>
#include <stop_token>

#include "agent/reply.hpp"

namespace dagent::agent {

class ModelSession {
public:
    virtual ~ModelSession() = default;

    /// @brief 阻塞到拿到完整回复。on_event 在调用线程上收到每个 StreamEvent（含 Usage、Finish）；
    /// on_retry 在每次重试等待之前调用。
    virtual Reply complete(const Request& request,
                           const std::function<void(const StreamEvent&)>& on_event,
                           const std::function<void(const RetryInfo&)>& on_retry,
                           std::stop_token stop) = 0;
};

} // namespace dagent::agent
