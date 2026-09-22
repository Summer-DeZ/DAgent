/// @file reply.hpp
/// @brief 模型调用的中立值：流式增量、Usage、Finish、完整回复、重试信息与错误分类。
///
/// 这些类型不含 HTTP/SSE/厂商协议；编解码器（llm 模块）负责在它们与网络报文之间翻译。
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "agent/message.hpp"

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

struct Reply {
    Message message; ///< role = assistant
    Finish finish;
    std::optional<Usage> usage;
};

struct RetryOptions {
    int max_retries = 2; ///< 总尝试次数 = 1 + max_retries
    std::chrono::milliseconds base_delay{1000};
    std::chrono::milliseconds max_delay{30000};
    std::chrono::milliseconds max_retry_after{300000}; ///< 服务端要求等更久就不等了
};

/// @brief 一次重试给人看的信息。
struct RetryInfo {
    int attempt = 0, max_attempts = 0; ///< 第几次重试（从 1 开始）、最多几次
    std::chrono::milliseconds wait{0};
    std::string reason; ///< 给人看：「HTTP 503」「连接中断」「流没有正常结束」
    bool had_output = false; ///< 这次失败的尝试是否已经交出过事件（决定要不要 StreamReset）
};

/// @brief 模型调用的失败分类；partial() 是这次尝试已收到的内容。
class ModelError : public std::runtime_error {
public:
    enum class Kind {
        cancelled,        ///< stop_token；partial() 是这次尝试已收到的内容
        context_too_long, ///< 编解码分类判定上下文超长
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

} // namespace dagent::agent
