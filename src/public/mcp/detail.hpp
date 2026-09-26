/// @file detail.hpp
/// @brief mcp 内部跨实现文件共享的细节，不属于对外接口；外部代码不要 include。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcp/client.hpp"

namespace dagent::mcp::detail {

/// @brief 当前实现的现代协议版本。
inline constexpr std::string_view kModernVersion = "2026-07-28";
/// @brief server 声明的版本数组是否包含当前协议。
bool supports_protocol(const nlohmann::json& supported);

/// @brief 组装现代协议每个请求都要带的 params._meta。
nlohmann::json modern_meta();

nlohmann::json client_info();

nlohmann::json make_request(int64_t id, std::string_view method, nlohmann::json params);
nlohmann::json make_notification(std::string_view method, nlohmann::json params);

/// @brief 把任意名字清理成 [A-Za-z0-9_-]，其余字符换成 '_'。
std::string sanitize(std::string_view s);
std::string qualified_name(std::string_view server, std::string_view tool);

/// @brief 一个 x-mcp-header 注解：参数路径 → Mcp-Param-<header> 请求头。
struct HeaderParam {
    std::string header;
    std::vector<std::string> path;
};

/// @brief 从工具的 inputSchema 收集 x-mcp-header 注解。schema 不合规时返回 false 并填 reason，
/// 调用方应把该工具从 tools/list 里剔除（现代协议的要求）。
bool collect_header_params(const nlohmann::json& schema, std::vector<HeaderParam>& out, std::string& reason);

/// @brief 按注解从调用参数里取值并编码成请求头；缺值、null 或类型不符的跳过。
std::vector<std::pair<std::string, std::string>> header_param_values(const std::vector<HeaderParam>& params,
                                                                     const nlohmann::json& arguments);

/// @brief HTTP 头值编码：可见 ASCII 原样，其余（含首尾空白、哨兵串）用 =?base64?...?= 包装。
std::string encode_header_value(std::string_view value);

} // namespace dagent::mcp::detail
