/// @file model.hpp
/// @brief 一次模型调用：中立 Request → 流式 HTTP → 中立 Reply，含累积、失败分类与重试。
///
/// 不含请求内容（Conversation）、压缩策略（docs/design/agent.md §8）与参数 JSON 解析（tools 层）。
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>

#include "agent/provider.hpp"
#include "agent/message.hpp"
#include "net/http.hpp"

namespace dagent::agent {

struct RetryOptions {
    int max_retries = 2; ///< 总尝试次数 = 1 + max_retries
    std::chrono::milliseconds base_delay{1000};
    std::chrono::milliseconds max_delay{30000};
    std::chrono::milliseconds max_retry_after{300000}; ///< 服务端要求等更久就不等了
};

struct Reply {
    Message message; ///< role = assistant
    Finish finish;
    std::optional<Usage> usage;
};

/// @brief 一次重试给人看的信息。
struct RetryInfo {
    int attempt = 0, max_attempts = 0; ///< 第几次重试（从 1 开始）、最多几次
    std::chrono::milliseconds wait{0};
    std::string reason; ///< 给人看：「HTTP 503」「连接中断」「流没有正常结束」
    bool had_output = false; ///< 这次失败的尝试是否已经交出过事件（决定要不要 StreamReset）
};

class ModelError : public std::runtime_error {
public:
    enum class Kind {
        cancelled,        ///< stop_token；partial() 是这次尝试已收到的内容
        context_too_long, ///< Codec::classify 判定上下文超长
        rejected,         ///< 不可重试：401/403/400/404、TLS、响应过大、网关不返回 SSE
        exhausted,        ///< 可重试的错误用完了重试次数，或 Retry-After 太长
    };

    ModelError(Kind kind, Reply partial, const std::string& what);

    Kind kind() const noexcept { return kind_; }
    const Reply& partial() const noexcept { return partial_; }

private:
    Kind kind_;
    Reply partial_;
};

class Model {
public:
    Model(std::function<std::unique_ptr<Codec>()> codec_factory, net::HttpOptions http, RetryOptions retry,
          Framing framing = Framing::sse);

    /// @brief 阻塞到拿到完整回复。on_event 在调用线程上收到每个 StreamEvent（含 Usage、Finish）；
    /// on_retry 在每次重试等待之前调用。
    Reply complete(const Request& request, const std::function<void(const StreamEvent&)>& on_event,
                   const std::function<void(const RetryInfo&)>& on_retry, std::stop_token stop);

private:
    struct AttemptOutcome {
        enum class Kind { success, retryable, cancelled, context_too_long, rejected };
        Kind kind = Kind::success;
        Reply reply;
        std::string message;
        std::chrono::milliseconds retry_after{0};
        bool had_output = false;
    };

    AttemptOutcome attempt(const Request&, const std::function<void(const StreamEvent&)>& on_event,
                           std::stop_token stop);

    std::function<std::unique_ptr<Codec>()> codec_factory_;
    net::HttpClient http_;
    RetryOptions retry_;
    Framing framing_;
    int call_serial_ = 0; ///< 给没有 id 的调用补 `call_<n>_<序号>`，在一个会话内唯一
};

} // namespace dagent::agent
