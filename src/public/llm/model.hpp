/// @file model.hpp
/// @brief 一次模型调用：中立 Request → 流式 HTTP → 中立 Reply，含累积、失败分类与重试。
///
/// llm::Model 实现 agent::ModelSession 端口；不含请求内容、压缩策略与参数 JSON 解析。
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

#include "agent/port_model.hpp"
#include "llm/provider.hpp"

namespace dagent::llm {

class Model final : public agent::ModelSession {
public:
    Model(std::function<std::unique_ptr<Codec>()> codec_factory, net::HttpOptions http,
          agent::RetryOptions retry, Framing framing = Framing::sse);

    /// @brief 阻塞到拿到完整回复。on_event 在调用线程上收到每个 StreamEvent（含 Usage、Finish）；
    /// on_retry 在每次重试等待之前调用。
    agent::Reply complete(const agent::Request& request,
                          const std::function<void(const agent::StreamEvent&)>& on_event,
                          const std::function<void(const agent::RetryInfo&)>& on_retry,
                          std::stop_token stop) override;

private:
    struct AttemptOutcome {
        enum class Kind { success, retryable, cancelled, context_too_long, rejected };
        Kind kind = Kind::success;
        agent::Reply reply;
        std::string message;
        std::chrono::milliseconds retry_after{0};
        bool had_output = false;
    };

    AttemptOutcome attempt(const agent::Request&,
                           const std::function<void(const agent::StreamEvent&)>& on_event,
                           std::stop_token stop);

    std::function<std::unique_ptr<Codec>()> codec_factory_;
    net::HttpClient http_;
    agent::RetryOptions retry_;
    Framing framing_;
    int call_serial_ = 0; ///< 给没有 id 的调用补 `call_<n>_<序号>`，在一个会话内唯一
};

/// @brief 装配入口：公开描述 + 密钥配置 → 已配置的模型客户端（agent::ModelSession）。
std::shared_ptr<agent::ModelSession> make_session(const ProviderConfig&, net::HttpOptions,
                                                  agent::RetryOptions);

} // namespace dagent::llm
