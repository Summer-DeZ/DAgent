#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "llm/codec.hpp"
#include "llm/provider.hpp"

// Provider 实现共用的报文原语，不进入 app 装配接口；只有 llm 模块的实现使用。
namespace dagent::llm::provider_detail {
std::string join_url(std::string_view, std::string_view);
std::string string_field(const nlohmann::json&, const char*);
int int_field(const nlohmann::json&, const char*);
std::string error_message(const nlohmann::json&);
std::string_view role_name(agent::Role);
nlohmann::json encode_tools(const std::vector<agent::ToolSpec>&);
Error classify_http(const net::HttpResponse&, std::string_view api_key);
void merge_extra(nlohmann::json&, const nlohmann::json&);
std::unique_ptr<Codec> make_chat(const ProviderConfig&);
std::unique_ptr<Codec> make_anthropic(const ProviderConfig&);
std::unique_ptr<Codec> make_ollama(const ProviderConfig&);
} // namespace dagent::llm::provider_detail
