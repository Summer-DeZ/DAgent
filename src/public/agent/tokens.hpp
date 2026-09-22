/// @file tokens.hpp
/// @brief 启发式 token 估算与按真实 usage 校正。纯字符串计算，无网络与厂商协议依赖。
#pragma once

#include <cstddef>
#include <string_view>

#include "agent/message.hpp"

namespace dagent::agent {

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
