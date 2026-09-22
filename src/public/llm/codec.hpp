/// @file codec.hpp
/// @brief LLM 编解码边界：Codec 接口与错误分类。不含重试、压缩、工具执行。
///
/// 中立值（流式事件、Usage、Finish、Reply、估算器）留在 agent；HTTP/SSE 类型只出现在
/// llm 模块。
#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "agent/reply.hpp"
#include "agent/tokens.hpp"
#include "net/http.hpp"
#include "net/sse.hpp"

namespace dagent::llm {

/// @brief 非 2xx 响应的分类结果。
struct Error {
    bool retryable = false;     ///< 换个时间重试同样的请求有意义
    bool context_too_long = false; ///< 400 且看起来是上下文超长，核心据此触发压缩
    std::chrono::milliseconds retry_after{0}; ///< 服务端要求的最短等待，0 表示没给
    std::string message;        ///< 面向用户的说明；绝不含密钥
};

/// @brief 厂商协议编解码器。有状态：一次请求用一个实例（decode 累积流式分片）。
class Codec {
public:
    virtual ~Codec() = default;

    /// @brief 中立请求 → HTTP 请求（URL、鉴权头、JSON body）。
    virtual net::HttpRequest encode(const agent::Request&) const = 0;

    /// @brief 吃一个 SSE 事件，把中立事件追加到 out；不抛异常。
    virtual void decode(const net::SseEvent&, std::vector<agent::StreamEvent>& out) = 0;

    /// @brief 非 2xx 响应 → 能不能重试、等多久。
    virtual Error classify(const net::HttpResponse&) const = 0;
};

} // namespace dagent::llm
