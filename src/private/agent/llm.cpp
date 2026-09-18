#include "agent/llm.hpp"

namespace dagent::agent {

std::size_t estimate_tokens(std::string_view text) {
    std::size_t ascii = 0;
    std::size_t wide = 0;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c < 0x80) {
            ++ascii;
        } else if ((c & 0xC0) != 0x80) {
            ++wide; // 每个多字节码点（CJK 等）约 1 个 token
        }
    }
    return (ascii + 3) / 4 + wide;
}

std::size_t estimate_prompt_tokens(const Request& request) {
    std::size_t total = 3; // 对话起始符一类的固定开销
    for (const Message& message : request.messages) {
        total += 4 + estimate_tokens(message.content) + estimate_tokens(message.tool_call_id);
        for (const ToolCall& call : message.tool_calls) {
            total += 8 + estimate_tokens(call.id) + estimate_tokens(call.name) + estimate_tokens(call.arguments);
        }
    }
    for (const ToolDef& tool : request.tools) {
        total += 8 + estimate_tokens(tool.name) + estimate_tokens(tool.description) +
                 estimate_tokens(tool.parameters.dump());
    }
    return total;
}

std::size_t TokenEstimator::estimate(const Request& request) {
    pending_ = estimate_prompt_tokens(request);
    return static_cast<std::size_t>(static_cast<double>(pending_) * factor_ + 0.5);
}

void TokenEstimator::observe_prompt_tokens(std::size_t actual) {
    if (pending_ == 0) return;
    const double ratio = static_cast<double>(actual) / static_cast<double>(pending_);
    factor_ = 0.5 * factor_ + 0.5 * ratio; // 用指数滑动平均吸收单次波动
    pending_ = 0;
}

} // namespace dagent::agent
