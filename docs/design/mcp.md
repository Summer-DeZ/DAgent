# mcp：MCP 客户端

连接外部 MCP server、发现并调用工具。头文件位于 `src/public/mcp/`，实现位于 `src/private/mcp/`，
构建为静态库 `dagent_mcp`，命名空间为 `dagent::mcp`。依赖 base、exec 的 `Child` 和 net 的
`HttpClient`、`SseParser`。

`mcp/detail.hpp` 仅供模块内部共享 JSON-RPC 组装、协议元数据、工具命名和 HTTP 参数头处理。
`mcp/transport.hpp` 定义内部 `Response`、`ConnectionState`、`StdioTransport` 和 `HttpTransport`，
同样不属于 Client 对外接口。

## 1. 接口与生命周期

| 接口 | 行为 |
| --- | --- |
| `Client::connect` | 启动传输、确认协议版本、拉取一次 `tools/list`；失败时清理连接并抛 `McpError` |
| `tools()` | 返回连接时发现的工具，连接存续期间保持不变 |
| `protocol_version()` | 返回本连接协商确定的协议版本；连接时也写入 info 日志与 MCP 状态输出的 `protocol_version` |
| `call` | 用 `qualified_name` 调用工具；`isError` 属于正常工具结果 |

一个 Client 对应一个 server。`call` 在调用线程阻塞；stdio 请求共享子进程并按 id
分发响应，HTTP 请求各自创建连接。

`tools::McpHub` 在后台连接 server，主会话在模型请求之间等待连接并合并工具。工具结果表明连接断开时，
Hub 自动重连一次。子会话使用创建时的工具快照，工具项持有 Client 的共享所有权。
合并时在 Hub 锁内接收连接并保留局部强引用，锁外注册工具只使用该引用；并发撤权可使连接失效，
但不会使注册过程访问已被清空的指针。

## 2. 协议发现

优先使用 `2026-07-28`。现代协议每个请求的 `params._meta` 包含：

- `io.modelcontextprotocol/protocolVersion`
- `io.modelcontextprotocol/clientInfo`
- `io.modelcontextprotocol/clientCapabilities`

`connect` 先请求 `server/discover`，要求成功结果中的 `supportedVersions` 包含当前版本。
仅当发现请求返回 JSON-RPC `-32601`（Method not found），或 HTTP 404/405 时，回退到旧版
`initialize`。鉴权失败、HTTP 5xx、超时、传输错误、成功结果缺少当前版本及其他协议错误保持原有失败行为。

旧版握手发送 `protocolVersion: "2025-11-25"`、空 `capabilities` 和现有 `clientInfo`；接受服务端协商的
`2025-11-25`、`2025-06-18` 或 `2025-03-26`，其他版本以包含版本值的 `handshake` 错误拒绝。
随后发送不带请求 id 的 `notifications/initialized`，再按原有分页规则获取 `tools/list`。
旧版请求不注入上面的三个现代 `_meta` 字段，保留既有参数及共享元数据（如 progress token）；
stdio 超时和取消仍发送 `notifications/cancelled`。协商由共享客户端实现，managed stdio 经 SRT bridge
同样执行这套握手，无需修改 bridge。

stdio 的发现和初始化各使用 `probe_timeout`，HTTP 各使用 `connect_timeout`；超时直接失败，不触发回退。

stdio server 冷启动时间包含在发现超时内，调用方需要按真实启动耗时配置 `probe_timeout`。

### 兼容矩阵与内部结构

| 传输 | 修订 | 握手 | 请求 `_meta` | HTTP 会话 | `MCP-Protocol-Version` | HTTP 路由头 |
| --- | --- | --- | --- | --- | --- | --- |
| stdio（SRT bridge） | `2026-07-28` | `server/discover` | 注入 | — | — | — |
| stdio（SRT bridge） | `2025-11-25` | `initialize` → `notifications/initialized` | 不注入 | — | — | — |
| stdio（SRT bridge） | `2025-06-18` | `initialize` → `notifications/initialized` | 不注入 | — | — | — |
| stdio（SRT bridge） | `2025-03-26` | `initialize` → `notifications/initialized` | 不注入 | — | — | — |
| Streamable HTTP | `2026-07-28` | `server/discover` | 注入 | 无 | 发送 | `Mcp-Method`、`Mcp-Name`、`Mcp-Param-*` |
| Streamable HTTP | `2025-11-25` | `initialize` → `notifications/initialized` | 不注入 | 可选 session id，关闭时 DELETE | 发送 | 无 |
| Streamable HTTP | `2025-06-18` | `initialize` → `notifications/initialized` | 不注入 | 可选 session id，关闭时 DELETE | 发送 | 无 |
| Streamable HTTP | `2025-03-26` | `initialize` → `notifications/initialized` | 不注入 | 可选 session id，关闭时 DELETE | 不发送 | 无 |

矩阵描述协商完成后的行为；旧版初始化请求仍提议首选 `2025-11-25` 并按该修订组装请求，
收到协商结果后，初始化完成通知、工具请求和 DELETE 均使用选定修订。
`Mcp-Name` 和 `Mcp-Param-*` 仅用于工具调用，后者还取决于 schema 注解。

协议类型均留在 `client.cpp` 内部：`Revision` / `kRevisions` 集中声明版本、dialect、握手方法、
请求元数据、HTTP 会话、版本头和路由头能力，同一 dialect 内按表中顺序选择首选修订。
`Dialect` 负责请求参数和请求头装饰，并提供握手、响应头捕获、会话过期和关闭接口；
`StatelessDialect` 实现发现握手，`SessionDialect` 实现初始化握手并持有 session id。
`Impl::negotiate` 按无状态、会话型顺序尝试；只有明确不支持握手的返回值才进入下一 dialect，
其他失败直接传播。选定修订在 dialect 中保存，成功后记录版本并写日志。
stdio/SRT 的收发、pending 请求匹配、超时及取消由 `stdio_transport.cpp` 中的 `StdioTransport` 负责；
HTTP 请求构造、SSE 解析、计时/取消、响应匹配与解包及 DELETE 位于 `http_transport.cpp` 中的 `HttpTransport`。
请求编号、参数装饰、Dialect、协议版本、工具目录、参数头映射及最终错误分类留在 Impl。
Impl 持有共享 `ConnectionState`，传输借用它及不可变的配置/选项，不持有 Impl、不参与协议协商或重连。
普通请求的参数由 Dialect 装饰后交给传输，通知仍不作参数装饰。
客户端及两种传输统一直接使用 `base::logger("mcp")`。

新增采用既有握手模型的修订时，在 `kRevisions` 中按偏好增加一条能力记录即可；版本匹配、
元数据和 HTTP 头均读取该记录。只有握手模型改变时才增加 dialect 实现，并加入协商偏好顺序。

## 3. 传输

### stdio

连接定义来自 `home/config/mcp.json` 的 `mcpServers`。默认 `environment: managed` 使用
托管工具和干净环境；`mcp/<name>` 使用 runtime sync 准备的独立依赖，cwd 为 home。
显式 `project` 使用宿主工具及工作区 cwd。server 的 `env` 最后覆盖环境变量。
工具准备、锁文件和版本生命周期见 [toolchain](toolchain.md)。

stdio server 必须携带受信任配置里的显式权限 profile（`permissions.read/write/network`）；缺少 profile
时不会启动，不允许"先运行、再在 tools/call 时请求批准"。启动方式是在 SRT 内运行：每连接一个长生命周期
bridge，server 只拿到 profile 允许的读取/写入范围与严格网络 allowlist（`strictAllowlist=true`，没有运行中
审批），私有 HOME/TMPDIR 使用每次连接独占的 `/tmp/dagent-home-XXXXXX`（`mkdtemp` 创建，0700），
避免 Home 路径或 server 名过长导致 SRT Unix socket 超过路径上限。`init.json` 仍保存在 state root 下的
每连接状态目录；连接关闭或启动失败时，在子进程结束后清理状态目录与私有 HOME。
新旧连接与 lease 在权限降级或切换时被显式作废
（`McpHub::revoke`），旧 `Client` 的后续请求直接返回 `disconnected`。

bridge 在 stderr 上以 `{"dagent_bridge":true,...}` 帧报告 ready/error/exited；server 自身的 stderr
原样转发为日志。调用线程登记请求 id 后发送消息，读取线程按响应 id 唤醒等待者。超时、取消与子进程退出都会
清除等待项；迟到响应直接丢弃。客户端不处理 server 主动请求或通知。
stderr 回调在获取握手锁前注册，允许同步回放已缓存的 ready/error 帧。发送和关闭时移走 Child 使用同一把锁，
关闭在释放锁后终止子进程并等待读取线程退出，避免回调重新加锁时死锁。
这把锁位于 Impl 持有的 `ConnectionState`；stdio 的 pending 登记、发送、退出通知和关闭均使用它。
Impl 在销毁传输前调用 `StdioTransport::shutdown`：锁内标记关闭、移走 Child、失败未完成的 pending，
锁外唤醒等待者，再经 `CloseProtocol` 回调执行原 Dialect 关闭，随后 terminate/join 并清理目录。
启动失败也沿用这条清理路径，传输及其借用的连接状态在所有进程回调结束前保持存活。
Hub 处理连接失败时，除向前端发送 notice 外，也将具体失败原因写入后端 warning 日志。

### Streamable HTTP

每次调用独占 `HttpClient`，并发调用互不排队。连接超时、响应体上限和 TLS 校验来自 `Options::http`；
总时长由调用的 `timeout` 参数控制，`http.timeout` 不生效。HTTP 连接不跨调用复用。

Impl 先构造 JSON-RPC 消息，再同步调用 `HttpTransport::exchange`。传输在创建 HttpClient 与请求对象后
调用 `HeaderFactory` 获取协议请求头，在响应头回调中记录 HTTP 状态后调用 `HeaderObserver`，
由 Dialect 当场采集 session id；两个回调仅在本次 exchange 内使用，不保存为成员。
HttpClient、计时器、stop relay、SSE parser、body 和响应匹配状态仍是每次请求的局部对象。
正常返回分支仍按非 2xx、通知、正文解析与匹配结果处理；HttpError 分支依次检查已匹配响应、
调用方取消、总时限、body 上限和底层错误种类。HTTP 取消不发送 `notifications/cancelled`。

请求头包含 `Accept: application/json, text/event-stream`；除 `2025-03-26` 外，
还包含 `MCP-Protocol-Version: <协商版本>`（该头自 `2025-06-18` 引入）。
现代协议额外发送 `Mcp-Method`。整包 JSON 与 SSE 响应均可处理；收到对应 id 的 SSE 响应后立即关闭流。

旧版 HTTP 保存 `initialize` 响应中的可选 `Mcp-Session-Id`，后续请求（包括初始化完成通知）携带该会话头
及该修订支持的版本头。响应头在读取正文前保存，SSE 提前关闭也不会丢失 session id；没有会话头的服务端仍可使用。
持有会话时收到 HTTP 404 会使 Client 断开，后续调用拒绝，由 Hub 沿用现有重连流程。
关闭持有会话的连接时向原 endpoint 发送带会话头及该修订支持的版本头的 HTTP DELETE，最长等待 5 秒；
服务端已清理会话（404）或不支持删除（405）不影响本地关闭。现代协议不创建或删除 HTTP 会话。
关闭时先在连接状态锁内标记 closing/disconnected，再在锁外经 `CloseProtocol` 调用 Dialect 关闭；
`SessionDialect::close` 经 Impl 直接调用 `HttpTransport::close_session`，DELETE 不经过普通 exchange 的关闭检查。

现代协议的 `tools/call` 额外发送 `Mcp-Name`，并按照工具 schema 的 `x-mcp-header` 注解生成 `Mcp-Param-*`。
非可见 ASCII、首尾空白或哨兵形式的头值使用 `=?base64?…?=` 编码。integer 参数必须在 ±(2^53−1)
范围内且可精确表示，`42.0` 也可转换。

`x-mcp-header` 只能位于从根开始的 `properties` 链上，参数类型限于 string、integer、boolean。
头名必须合法且不区分大小写地唯一。不合规工具被剔除并记录警告，不影响同一 server 的其他工具。
旧版协议不校验此注解，也不生成 `Mcp-Method`、`Mcp-Name` 或 `Mcp-Param-*` 头。

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
| `disconnected` | 子进程退出、HTTP 连接失败或中断、HTTP 5xx、旧版 HTTP 会话过期（404） |

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
不支持 `2024-11-05` 的旧 HTTP+SSE 传输。

app 负责解析 `home/config/mcp.json` 的 `mcpServers` 并展开 `${VAR}`，包括每个 server 的
`permissions` profile。`mcp.connect_timeout_ms` 和 `mcp.probe_timeout_ms` 控制发现和调用启动超时，
进程与 HTTP 选项沿用对应配置段；stdio 的 SRT 启动握手使用 `sandbox.startup_timeout_ms`。
`clientInfo.version` 使用根 CMake 项目的 `DAGENT_VERSION` 编译宏。

HTTP server 的连接目标只来自受信任配置的 `url`，且要求 profile 显式列出网络目标；客户端不跟随重定向
（3xx 按协议错误处理），认证头不会转发到任何未批准端点。审计与界面把 HTTP server 标为 `remote`，
不把本机 SRT 保证扩大到远端服务；本地 stdio server 标为 `srt`。
