# mcp：MCP 客户端

连接外部 MCP server、发现并调用工具。头文件位于 `src/public/mcp/`，实现位于 `src/private/mcp/`，
构建为静态库 `dagent_mcp`，命名空间为 `dagent::mcp`。依赖 base、exec 的 `Child` 和 net 的
`HttpClient`、`SseParser`。

`mcp/detail.hpp` 仅供模块内部共享 JSON-RPC 组装、协议元数据、工具命名和 HTTP 参数头处理。

## 1. 接口与生命周期

| 接口 | 行为 |
| --- | --- |
| `Client::connect` | 启动传输、确认协议版本、拉取一次 `tools/list`；失败时清理连接并抛 `McpError` |
| `tools()` | 返回连接时发现的工具，连接存续期间保持不变 |
| `call` | 用 `qualified_name` 调用工具；`isError` 属于正常工具结果 |

一个 Client 对应一个 server。`call` 在调用线程阻塞；stdio 请求共享子进程并按 id
分发响应，HTTP 请求各自创建连接。

`tools::McpHub` 在后台连接 server，主会话在模型请求之间等待连接并合并工具。工具结果表明连接断开时，
Hub 自动重连一次。子会话使用创建时的工具快照，工具项持有 Client 的共享所有权。

## 2. 协议发现

仅支持 `2026-07-28`。每个请求的 `params._meta` 包含：

- `io.modelcontextprotocol/protocolVersion`
- `io.modelcontextprotocol/clientInfo`
- `io.modelcontextprotocol/clientCapabilities`

`connect` 请求 `server/discover`，要求成功结果中的 `supportedVersions` 包含当前版本。缺少当前版本、
JSON-RPC 拒绝或没有发现结果都会导致连接失败。stdio 使用 `probe_timeout`，HTTP 使用
`connect_timeout`；超时直接失败。客户端不发送 `initialize`，不维护 HTTP 协议会话，也不回退到旧协议。

stdio server 冷启动时间包含在发现超时内，调用方需要按真实启动耗时配置 `probe_timeout`。

## 3. 传输

### stdio

子进程通过 `exec::Child` 启动，进程环境和结束宽限使用 `exec::Options`；`ServerConfig::env`
显式注入的变量不受环境过滤。stderr 由进程层交付日志。

调用线程登记请求 id 后发送消息，读取线程按响应 id 唤醒等待者。超时、取消与子进程退出都会清除等待项；
迟到响应直接丢弃。客户端不处理 server 主动请求或通知。

### Streamable HTTP

每次调用独占 `HttpClient`，并发调用互不排队。连接超时、响应体上限和 TLS 校验来自 `Options::http`；
总时长由调用的 `timeout` 参数控制，`http.timeout` 不生效。HTTP 连接不跨调用复用。

请求头包含 `Accept: application/json, text/event-stream`、`MCP-Protocol-Version: 2026-07-28`
和 `Mcp-Method`。整包 JSON 与 SSE 响应均可处理；收到对应 id 的 SSE 响应后立即关闭流。

`tools/call` 额外发送 `Mcp-Name`，并按照工具 schema 的 `x-mcp-header` 注解生成 `Mcp-Param-*`。
非可见 ASCII、首尾空白或哨兵形式的头值使用 `=?base64?…?=` 编码。integer 参数必须在 ±(2^53−1)
范围内且可精确表示，`42.0` 也可转换。

`x-mcp-header` 只能位于从根开始的 `properties` 链上，参数类型限于 string、integer、boolean。
头名必须合法且不区分大小写地唯一。不合规工具被剔除并记录警告，不影响同一 server 的其他工具。

## 4. 超时、取消与错误

| 情况 | stdio | HTTP |
| --- | --- | --- |
| 超时 | 抛 `timeout`，发送 `notifications/cancelled` | 抛 `timeout`，关闭响应流 |
| `stop_token` 取消 | 抛 `cancelled`，发送取消通知 | 抛 `cancelled`，关闭响应流 |
| 断连 | 子进程退出后等待中的调用及后续调用抛 `disconnected` | 连接失败或中断抛 `disconnected` |

超时、取消后 Client 可继续使用；stdio 子进程退出后需要重新连接。

| `McpError::Kind` | 含义 |
| --- | --- |
| `spawn` | stdio 子进程启动失败 |
| `handshake` | 协议发现失败、版本不受支持，或 HTTP 401/403 |
| `timeout` | 等待响应超时 |
| `cancelled` | 调用方停止 |
| `rpc` | 工具发现或调用返回 JSON-RPC error |
| `protocol` | 消息不符合协议、未知工具、不支持的结果或非 JSON-RPC 的其他 4xx |
| `disconnected` | 子进程退出、HTTP 连接失败或中断、HTTP 5xx |

## 5. 工具与结果

工具名使用 `mcp__<server>__<tool>`，其中非 `[A-Za-z0-9_-]` 字符替换为 `_`。超过 64 字符或同一
server 内清理后重名的工具被跳过。跨 server 的名字冲突由 app 在加载配置时检查。

`tools/list` 按 `nextCursor` 分页，游标不变或达到分页上限时停止并记录警告。
`CallResult::content` 保留内容块；`structured` 保存 `structuredContent`；`is_error` 对应 `isError`。
`resultType: "input_required"` 需要 MRTR，本模块不支持并抛 `protocol`。

## 6. 范围与配置

仅实现 tools，不实现 resources、prompts、sampling、elicitation、MRTR、HTTP OAuth 或
`subscriptions/listen`。工具列表在连接时发现，重新连接时重新获取；没有服务端通知刷新
和周期刷新。鉴权头通过 `ServerConfig::headers` 提供。

app 负责解析 `config.json` 中的 `mcp.servers` 并展开 `${VAR}`。`mcp.connect_timeout_ms` 和
`mcp.probe_timeout_ms` 控制发现和调用启动超时，进程与 HTTP 选项沿用对应配置段。
`clientInfo.version` 使用根 CMake 项目的 `DAGENT_VERSION` 编译宏。
