/// @file rpc.hpp
/// @brief JSON-RPC 2.0 消息编解码与固定错误码；一行一个完整对象，不支持 batch。
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "lib/nlohmann/json.hpp"

namespace dagent::protocol {

namespace rpc_code {
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kBusinessError = -32000; ///< 业务错误；data.kind 指出类别
} // namespace rpc_code

struct RpcError {
    int code = rpc_code::kInternalError;
    std::string message;
    std::string kind;     ///< data.kind；空表示非业务错误
    nlohmann::json data = nlohmann::json::object();
};

struct Request {
    std::string id;
    std::string method;
    nlohmann::json params = nlohmann::json::object();
};

struct Response {
    std::string id;
    nlohmann::json result = nlohmann::json(nullptr);
    std::optional<RpcError> error;
};

struct Notification {
    std::string method;
    nlohmann::json params = nlohmann::json::object();
};

struct Message {
    enum class Kind { request, response, notification, invalid };
    Kind kind = Kind::invalid;
    Request request;
    Response response;
    Notification notification;
    std::string error; ///< invalid 时的说明
};

std::string encode_request(const Request&);
std::string encode_result(const std::string& id, nlohmann::json result);
std::string encode_error(const std::string& id, const RpcError&);
std::string encode_notification(const Notification&);

/// @brief 解析一行；无法识别时返回 Kind::invalid 与说明。
Message parse_message(std::string_view line);

} // namespace dagent::protocol
