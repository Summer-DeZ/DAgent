#pragma once

#include "agent/provider.hpp"

// Provider 实现共用的报文原语，不进入 Setup 或 app 的接口。
namespace dagent::agent::provider_detail {
std::string join_url(std::string_view, std::string_view);
std::string string_field(const nlohmann::json&, const char*);
int int_field(const nlohmann::json&, const char*);
std::string error_message(const nlohmann::json&);
std::string_view role_name(Role);
nlohmann::json encode_tools(const std::vector<ToolDef>&);
Error classify_http(const net::HttpResponse&, std::string_view api_key);
void merge_extra(nlohmann::json&, const nlohmann::json&);
std::unique_ptr<Codec> make_chat(const ProviderConfig&);
std::unique_ptr<Codec> make_anthropic(const ProviderConfig&);
std::unique_ptr<Codec> make_ollama(const ProviderConfig&);
} // namespace dagent::agent::provider_detail
