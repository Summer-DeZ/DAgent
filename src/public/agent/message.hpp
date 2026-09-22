/// @file message.hpp
/// @brief 核心的中立消息模型：与厂商协议无关，编解码器负责翻译成各自的 JSON。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "lib/nlohmann/json.hpp"

namespace dagent::agent {

enum class Role { system, user, assistant, tool };

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments; ///< 原始 JSON 字符串；可能是坏的，由核心决定怎么处理
};

struct Message {
    Role role = Role::user;
    std::string content;
    std::string reasoning_content;       ///< assistant 的思考内容；是否回传由编解码器选项决定
    std::string reasoning_signature;    ///< 思考回传时的校验串；没有则为空
    std::vector<ToolCall> tool_calls;    ///< role == assistant 时有效
    std::string tool_call_id;            ///< role == tool 时有效
};

struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json parameters = nlohmann::json::object(); ///< JSON Schema
};

/// 过渡别名：旧的模型侧工具描述与 ToolSpec 同字段（R13 删除清单）。
using ToolDef = ToolSpec;

/// @brief 构造请求的模型参数；不含密钥、HTTP 或 provider 配置。
struct ModelParams {
    std::string model;
    std::size_t max_tokens = 0; ///< 0 表示不发送，交给服务端默认值
    double temperature = -1.0;  ///< < 0 表示不发送
};

struct Request {
    std::string model;
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    std::size_t max_tokens = 0;   ///< 0 表示不发送，交给服务端默认值
    double temperature = -1.0;    ///< < 0 表示不发送
    bool stream = true;
};

} // namespace dagent::agent
