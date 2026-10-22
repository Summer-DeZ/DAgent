/// @file mcp_state.hpp
/// @brief MCP server 状态的中立值类型：核心、runtime 快照与协议适配共用，不含 MCP Client 实现。
#pragma once

#include <cstddef>
#include <string>

namespace dagent::agent {

struct McpServerState {
    std::string name;
    enum class Status { connecting, ready, failed, disconnected, reconnecting };
    Status status = Status::connecting;
    std::size_t tools = 0;
    std::string error;
    /// 隔离边界："mxc"（本机受限进程）或 "remote"（远端服务）；远程不受本机沙盒保护。
    std::string boundary;
    std::string protocol_version; ///< 当前连接协商的版本；未连接时为空
};

} // namespace dagent::agent
