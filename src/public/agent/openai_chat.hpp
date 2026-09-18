/// @file openai_chat.hpp
/// @brief OpenAI Chat Completions 编解码器：本地网关、vLLM、Ollama、DeepSeek 都兼容这个协议。
#pragma once

#include <memory>
#include <string>

#include "agent/llm.hpp"

namespace dagent::agent {

struct OpenAiChatOptions {
    std::string base_url; ///< 例如 http://127.0.0.1:10000/v1 或 https://api.deepseek.com/v1
    std::string api_key;  ///< 由 app 从 base::Secrets 读出后传入；不会出现在日志或错误信息里

    /// DeepSeek：历史里的 reasoning_content 要不要回传。早期版本回传会报 400，
    /// 后来的版本在工具调用的中间轮次要求回传，按当前官方文档设。
    bool send_reasoning_content = false;

    /// 请求体里加 stream_options.include_usage=true，保证流式请求也能拿到 usage（需要网关支持）。
    bool include_usage = true;

    /// 追加到请求体的厂商/网关专属字段（例如本地 Qwen 网关的 enable_thinking）；
    /// 已由 Request 生成的字段不会被覆盖。
    nlohmann::json extra_body = nlohmann::json::object();
};

std::unique_ptr<Codec> make_openai_chat_codec(OpenAiChatOptions options);

} // namespace dagent::agent
