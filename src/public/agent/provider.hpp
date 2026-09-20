#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include "agent/llm.hpp"

namespace dagent::agent {

struct ProviderConfig {
    std::string kind = "openai-chat";
    std::string name;
    std::string base_url;
    std::string model;
    std::string api_key; ///< app 解析密钥；不进入记录
    std::size_t max_tokens = 0;
    double temperature = -1.0;
    std::size_t context_window = 0;
    bool send_reasoning_content = false;
    bool include_usage = true;
    nlohmann::json extra_body = nlohmann::json::object();
};

enum class Framing : std::uint8_t { sse, ndjson };
struct ProviderInfo {
    std::string_view kind;
    std::string_view default_base_url;
    Framing framing = Framing::sse;
    bool needs_api_key = false;
    bool needs_max_tokens = false;
};

std::span<const ProviderInfo> providers() noexcept;
const ProviderInfo* find_provider(std::string_view kind) noexcept;
std::unique_ptr<Codec> make_codec(const ProviderConfig&);

} // namespace dagent::agent
