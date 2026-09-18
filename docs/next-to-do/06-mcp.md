# 06 mcp：MCP 客户端

库 `dagent_mcp`，命名空间 `dagent::mcp`。依赖 base、exec（`Child`，用于 stdio 传输）、net（用于 HTTP
传输）。里程碑 **M4**。

它同时依赖 exec 和 net，放进其中任何一个都会让那个模块多出一份依赖，所以单独成为一个模块。

## 职责

连接外部 MCP server，发现它们提供的工具并调用。核心拿到的是「一组带 JSON Schema 的工具，以及一个调用
函数」，和内置工具的形状一样。

不做（第一版）：resources、prompts、sampling、elicitation；HTTP server 的 OAuth 授权流程。

## 依赖

没有新依赖。MCP 就是 JSON-RPC 2.0，下面走两种传输方式：

| 传输 | 依赖 |
| --- | --- |
| stdio：启动子进程，每行一条 JSON 消息 | `exec::Child`（已完成，见 [exec 设计文档](../design/exec.md) 第 3 节） |
| Streamable HTTP：POST 请求，响应可能是 JSON，也可能是 SSE | `net::HttpClient` + `net::SseParser` 直接复用 |

目前没有成熟的官方 C++ SDK，协议本身又不大，所以自己实现。**协议版本以 modelcontextprotocol.io 上最新
的 spec 为准**，在 `initialize` 握手时协商。

## 接口草图

```cpp
namespace dagent::mcp {

struct ServerConfig {
    std::string name;
    std::vector<std::string> command;                        // stdio
    std::vector<std::pair<std::string, std::string>> env;
    std::string url;                                         // http
    net::Headers headers;
};

struct Tool {
    std::string server, name;               // 暴露给模型的名字：mcp__<server>__<name>
    std::string description;
    nlohmann::json input_schema;
};

struct CallResult {
    std::vector<nlohmann::json> content;    // text / image / resource 等内容块，原样交给核心
    bool is_error;
};

class Client {                              // 对应一个 server
public:
    static std::unique_ptr<Client> connect(const ServerConfig&, std::chrono::milliseconds timeout,
                                           std::stop_token = {});   // 连接并完成 initialize 握手
    const std::vector<Tool>& tools() const;                       // 已处理分页
    CallResult call(std::string_view tool, const nlohmann::json& args, std::chrono::milliseconds timeout,
                    std::stop_token = {});
    void on_tools_changed(std::function<void()>);                 // 收到 notifications/tools/list_changed 时触发
};

class McpError : public std::runtime_error { /* Kind: spawn / handshake / timeout / rpc / disconnected */ };
}
```

配置格式**沿用 Claude Code 和其他客户端通用的 `.mcp.json`**，用户现成的配置可以直接复制过来：

```json
{ "mcpServers": { "fs": { "command": "npx", "args": ["-y", "@modelcontextprotocol/server-filesystem", "."] },
                  "remote": { "type": "http", "url": "https://…/mcp", "headers": {"Authorization": "Bearer ${TOKEN}"} } } }
```

`${TOKEN}` 这类占位符由 app 层用 `base::Secrets` 展开。

## 实现要点

- **JSON-RPC 层**：用自增的请求 id，维护一张「id → promise」的表；读取线程收到响应后，按 id 找到对应的等待者并唤醒。`call` 在调用线程上阻塞等待。超时或取消时，要发一条 `notifications/cancelled`，并把这个 id 从表里删掉。
- **server 也会主动发请求**（比如 `ping`，或者我们没实现的 `roots/list`）：`ping` 要正常回复，其他未实现的方法回 `-32601 Method not found`。**不回复的话，有些 server 会一直卡住。**
- **stdio 的 stdout 只能包含协议消息**：server 的日志写在 stderr，由 `Child` 转到日志，不能混进协议流。
- **Streamable HTTP**：请求头要带 `Accept: application/json, text/event-stream`；响应头里有 `Mcp-Session-Id` 时，后续请求都要带上它；按 `content-type` 区分响应是 JSON 还是 SSE。
- `tools/list` 要处理 `nextCursor` 分页。
- 工具名要加命名空间前缀（`mcp__<server>__<tool>`），只保留 `[a-zA-Z0-9_-]`，因为 OpenAI 协议对工具名有这个限制。
- **一个 server 起不来，不能影响其他 server，也不能影响程序启动**：失败时只记日志，并在 UI 上给出提示。

## 验收（temp/mcp_check，用真实的 server）

本机有 `npx` 和 `uvx`，可以直接用官方的参考 server：

1. stdio：`npx -y @modelcontextprotocol/server-filesystem <目录>`，列出工具后调用 `read_text_file`，读到的内容和实际文件一致。
2. stdio：`uvx mcp-server-time`，调用 `get_current_time`。
3. 调用过程中 kill 掉 server：正在等待的 `call` 抛 `disconnected`，不会一直挂着。
4. 取消一个耗时较长的 call：server 能收到 `notifications/cancelled`（看 server 的 stderr 日志确认）。
5. HTTP：用参考 server 的 streamable-http 模式启动，走同样的测试流程。

## 审核关注点

id 表在超时、取消、断连三种情况下有没有清理；读取线程和调用线程之间的同步；server 主动发来的请求有没有回复。
