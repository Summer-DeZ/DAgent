# mcp：MCP 客户端

连接外部 MCP server、发现并调用它们提供的工具。头文件在 `src/public/mcp/`，实现在 `src/private/mcp/`，
构建为静态库 `dagent_mcp`，命名空间 `dagent::mcp`。依赖 base、exec（`Child`，stdio 传输）和 net
（`HttpClient` + `SseParser`，Streamable HTTP 传输）；同时依赖 exec 和 net，所以单独成为一个模块。

核心拿到的是「一组带 JSON Schema 的工具，以及一个调用函数」，和内置工具的形状一样。只做 tools：
resources、prompts、sampling、elicitation 都不做（见第 7 节）。

`mcp/detail.hpp` 放的是模块内部几个实现文件共用的代码（JSON-RPC 组装、名字清理、x-mcp-header 校验与
头值编码），外部不要 include。

---

## 1. 概述

```cpp
mcp::ServerConfig cfg{.name = "fs", .transport = mcp::Transport::stdio,
                      .command = {"npx", "-y", "@modelcontextprotocol/server-filesystem", root}};
auto client = mcp::Client::connect(cfg, options, stop);        // 协商协议 + 拉取一次 tools/list
for (const mcp::Tool& t : client->tools()) { /* t.qualified_name, t.description, t.input_schema */ }
mcp::CallResult r = client->call("mcp__fs__read_text_file", {{"path", "a.txt"}}, 30s, stop);
```

| 接口 | 行为 |
| --- | --- |
| `Client::connect` | 启动传输、协商协议版本、拉取一次 `tools/list`；任一步失败都会清理连接并抛 `McpError` |
| `tools()` | 已发现的工具（已处理分页）；`refresh_tools()` 之后引用失效 |
| `refresh_tools` | 重新拉取 `tools/list` 并替换 |
| `call` | 用 `qualified_name` 调用工具；工具执行出错（`isError`）是正常结果，不抛异常 |
| `on_tools_changed` | 收到 `notifications/tools/list_changed` 时的回调（只有经典 server 会发，见第 7 节） |
| `protocol_version` | 协商出的版本：现代为 `2026-07-28`，经典为 `initialize` 的结果 |

- **阻塞调用，可以并发**：`call`、`refresh_tools` 在调用线程上阻塞；多个线程同时调用同一个 Client 是
  安全的，互不排队。
- 一个 Client 对应一个 server；**一个 server 起不来不影响其他 Client**，由核心决定是否提示用户。

Agent 已通过 `agent::McpHub` 接入这些接口：后台连接每个 server，只在模型请求之间注册或刷新工具；工具结果标记
disconnected 后自动重连一次，失败警告不影响其他服务。状态可在交互界面查看；核心生命周期细节见
[McpHub](../next-to-do/10-mcp.md)。

---

## 2. 协议版本与握手

MCP 有两个时代，本模块两边都支持：

| 时代 | 版本 | 形状 |
| --- | --- | --- |
| 现代 | `2026-07-28` | 无握手、无会话；每个请求在 `params._meta` 里自带 `io.modelcontextprotocol/protocolVersion`、`clientInfo`、`clientCapabilities` |
| 经典 | `2025-11-25`、`2025-06-18`、`2025-03-26`、`2024-11-05` | `initialize` 握手 + `notifications/initialized`；HTTP 上有 `Mcp-Session-Id` 会话 |

`connect` 先用现代方法 `server/discover` 探测，按结果决定时代（规则来自 spec 的 Backward Compatibility 一节）：

| 结果 | stdio | HTTP |
| --- | --- | --- |
| 返回 `DiscoverResult` | 从 `supportedVersions` 选 `2026-07-28`；只有经典版本时走 `initialize` | 同左 |
| `UnsupportedProtocolVersionError`（-32022） | 从 `error.data.supported` 里选，规则同上 | 同左 |
| `HeaderMismatch`（-32020）/ `MissingRequiredClientCapability`（-32021） | — | 抛 `handshake`（请求本身有问题，不回退） |
| 404 + `-32601` | — | 现代 server 但没实现 discover，按现代协议继续 |
| 其他错误，或 `probe_timeout` 内没有回应 | 经典：走 `initialize` | 400/404/405 且不是可识别的现代错误 → 经典 |
| 401/403、5xx | — | 直接按状态码报错，不回退 |

**冷启动**：`npx`/`uvx` 第一次运行要下载依赖，现代 server 可能来不及在 `probe_timeout` 内回应 discover，
于是被当成经典 server 去 `initialize`。现代 server 会用 -32022 拒绝 `initialize` 并列出支持的版本，这时
改判为现代协议继续，不会连接失败。

经典握手时优先提出 `2025-11-25`；server 协商出的版本不在上面四个之内就抛 `handshake`。

---

## 3. 传输

### stdio

- 子进程由 `exec::Child` 启动：`setsid`、按 `exec::Options` 过滤环境变量，`ServerConfig::env` 注入的变量
  不受过滤；server 的 stderr 逐行写到 `base::logger("mcp")`。
- **请求 id → 等待者表**：调用线程登记后发送，读取线程收到响应按 id 唤醒；超时、取消、子进程退出三种情况
  都会把 id 从表里删掉。迟到的响应（请求已超时或取消）直接丢弃。
- **server 主动发来的请求**（只有经典协议会有）：`ping` 正常回复，其他方法回 `-32601 Method not found`。
  不回复的话，有些 server 会一直卡住。

### Streamable HTTP

- **每次调用各建一个 `HttpClient`**：并发调用互不排队，取消和超时立即生效；代价是连接不跨调用复用。
  `Options::http` 里的连接超时、响应体上限、TLS 校验照常生效，但 `http.timeout`（整个请求的总时长）
  **不生效**，总时长只由每次调用的 `timeout` 参数控制。
- 请求头带 `Accept: application/json, text/event-stream`；响应是整包 JSON 还是 SSE 都能处理。拿到本请求的
  响应后立即关闭流，不等 server 关闭（`initialize` 除外：它的响应头里有会话 id，必须读完流）。
- **现代协议的请求头**：每个请求带 `MCP-Protocol-Version: 2026-07-28` 和 `Mcp-Method`；`tools/call` 另带
  `Mcp-Name`，以及按工具 schema 里 `x-mcp-header` 注解镜像出来的 `Mcp-Param-*`。头值不是可见 ASCII
  （含首尾空白、非 ASCII、形如哨兵串）时编码成 `=?base64?…?=`；integer 参数只接受 ±(2^53−1) 以内、能精确
  表示的值（`42.0` 也算）。
- **x-mcp-header 校验**：注解只能出现在从根开始、完全经过 `properties` 的链上，类型只能是
  string/integer/boolean，头名要是合法的 HTTP 字段名且不区分大小写地唯一。不合规的工具从 `tools()` 里
  剔除并记 warn，不影响同一 server 的其他工具。
- **经典协议的会话**：只接受 `initialize` 响应里的 `Mcp-Session-Id`；协商版本 ≥ `2025-06-18` 时带
  `MCP-Protocol-Version`。

### 经典 HTTP 的会话过期

server 重启后会对旧会话回 404。带会话的请求收到 404 时：同一时刻只有一个线程重新 `initialize`，
其他线程发现会话已经换过就直接用新会话重试；每个请求只重试一次。新会话在 `notifications/initialized`
发送成功之后才对其他线程可见。server 重启后换了协议版本（或变成现代 server）时抛 `handshake`，需要核心
重新 `connect`。

---

## 4. 超时、取消与断连

| 情况 | stdio | 经典 HTTP | 现代 HTTP |
| --- | --- | --- | --- |
| 超时 | 抛 `timeout`，发 `notifications/cancelled` | 抛 `timeout`，另发一条 `notifications/cancelled` POST | 抛 `timeout`，关闭响应流即取消（spec 规定，不发通知） |
| `stop_token` 取消 | 抛 `cancelled`，同上 | 同上 | 同上 |
| 连接断开 | 子进程退出：等待中的调用立即抛 `disconnected`，之后的调用也直接抛 | 连接失败或中断抛 `disconnected` | 同左 |

取消和超时之后，同一个 Client 可以继续使用。stdio server 进程退出后这个 Client 就不能再用了，重启由核心
负责（重新 `connect`）。

---

## 5. 工具与调用结果

- **命名**：`qualified_name` 是 `mcp__<server>__<tool>`，server 名和工具名里 `[A-Za-z0-9_-]` 以外的字符
  都换成 `_`（OpenAI 协议对函数名的限制）。清理后超过 64 字符、或与同一 server 里前面的工具重名的，
  跳过并记 warn。**跨 server 的冲突**（`my.fs` 和 `my_fs`，或名字里含 `__`）本模块检测不到，由 app 在加载
  配置时保证。
- `tools/list` 按 `nextCursor` 分页；游标不变或超过 100 页时停止并记 warn。
- `CallResult`：`content` 是内容块数组，原样交给核心；`structured` 是 `structuredContent`（没有时为 null），
  只给结构化结果的 server 靠它传数据；`is_error` 对应 `isError`。
- 现代 server 返回 `resultType: "input_required"`（MRTR，要求补充输入）时抛 `protocol`：本模块没有实现。

---

## 6. 错误分类

`McpError::Kind` 按「调用方要不要换一种处理方式」划分：

| Kind | 含义 | 核心的典型处理 |
| --- | --- | --- |
| `spawn` | stdio 子进程启动失败（命令不存在、没有执行权限） | 提示配置有误 |
| `handshake` | 协议协商失败；任何阶段的 HTTP 401/403；会话恢复时 server 换了协议 | 提示用户检查配置/鉴权，或重新 connect |
| `timeout` | 等待响应超时 | 可以重试 |
| `cancelled` | `stop_token` 请求停止 | — |
| `rpc` | server 返回 JSON-RPC error | 交给模型或提示用户 |
| `protocol` | 消息不符合协议；未知工具；不支持的 `input_required`；响应不是 JSON-RPC 的其他 4xx | 视为 server 有问题 |
| `disconnected` | 子进程退出、HTTP 连接失败/中断、HTTP 5xx | 重新 connect 或稍后重试 |

按状态码报错时，响应体里如果带 JSON-RPC error，它的 message 会附在错误信息后面。

---

## 7. 已知限制

- 只做 tools。resources、prompts、sampling、elicitation、MRTR（`input_required`）都不做；现代 spec 已经
  把 roots、sampling、logging 标为弃用。
- 不实现 `subscriptions/listen`，所以**现代 server 的 `tools/list_changed` 收不到**，`on_tools_changed`
  只对经典 server 有效。需要时由核心定期 `refresh_tools`。
- 不做 HTTP server 的 OAuth 授权流程：鉴权头由 `ServerConfig::headers` 直接给出。
- 不支持已弃用的 HTTP+SSE 传输（2024-11-05）。
- HTTP 连接不跨调用复用。

---

## 8. 依赖与构建

- 没有第三方库，JSON-RPC 直接用 nlohmann 组装。`clientInfo.version` 取编译宏 `DAGENT_VERSION`
  （根 `CMakeLists.txt` 的 `PROJECT_VERSION`）。
- `.mcp.json` 的解析和 `${VAR}` 占位符展开属于 app，本模块只认 `ServerConfig`（见 [app 设计文档](app.md)）。
- 选项对应 `config/dagent.json` 的 `mcp` 段（`connect_timeout_ms`、`probe_timeout_ms`）；子进程和 HTTP
  部分沿用 `process`、`http` 段。
- 实测过的真实 server：`npx` 的 filesystem / everything、`uvx mcp-server-time`，以及用官方 Python SDK v1（经典）
  和 v2（现代）写的 server；复现需要本机有 `npx`、`uvx`、`uv`。
