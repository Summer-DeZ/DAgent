/// @file llm.hpp
/// @brief LLM 编解码：中立事件、错误分类、Codec 接口与 token 估算。不含重试、压缩、工具执行。
#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agent/message.hpp"
#include "net/http.hpp"
#include "net/sse.hpp"

namespace dagent::agent {

struct TextDelta {
    std::string text;
};
struct ReasoningDelta {
    std::string text;
    std::string signature = {}; ///< 可选的思考校验串增量
};
struct ToolCallBegin {
    int index = 0;
    std::string id, name;
};
struct ToolCallDelta {
    int index = 0;
    std::string args_fragment; ///< 参数 JSON 的片段，不能单独解析
};
struct ToolCallEnd {
    int index = 0;
};
struct Usage {
    int prompt = 0, completion = 0, cached = 0;
};
struct Finish {
    enum class Reason { stop, length, tool_calls, content_filter, error };
    Reason reason = Reason::error;
    std::string raw; ///< 服务端给的原始 finish_reason，便于排查
};
using StreamEvent =
    std::variant<TextDelta, ReasoningDelta, ToolCallBegin, ToolCallDelta, ToolCallEnd, Usage, Finish>;

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
    virtual net::HttpRequest encode(const Request&) const = 0;

    /// @brief 吃一个 SSE 事件，把中立事件追加到 out；不抛异常。
    virtual void decode(const net::SseEvent&, std::vector<StreamEvent>& out) = 0;

    /// @brief 非 2xx 响应 → 能不能重试、等多久。
    virtual Error classify(const net::HttpResponse&) const = 0;
};

/// @brief 启发式估算：ASCII 约 4 字节 1 token，非 ASCII 码点约 1 个 1 token。
std::size_t estimate_tokens(std::string_view text);

/// @brief 估算一个请求的 prompt tokens（不含历史 reasoning_content，是否发送由编解码器决定）。
std::size_t estimate_prompt_tokens(const Request& request);

/// @brief 用上一次的 usage.prompt_tokens 与估算值之比做校正，越用越准。
class TokenEstimator {
public:
    std::size_t estimate(const Request& request);
    void observe_prompt_tokens(std::size_t actual);

private:
    double factor_ = 1.0;
    std::size_t pending_ = 0;
};

} // namespace dagent::agent
