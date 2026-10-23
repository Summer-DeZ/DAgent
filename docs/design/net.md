# net：HTTP 客户端与 SSE 解析

`net` 模块（`dagent_net`）封装基于 libcurl 的 HTTP 传输与 WHATWG 规范的 Server-Sent Events（SSE）增量解析。

**职责边界**：
- 服务于**宿主侧受信任通信**：模型 API 请求、远程 HTTP MCP 调用、以及本地托管 SearXNG 的健康检查。
- **不包含模型协议语义**：报文格式与事件 JSON 由上层（`llm`、`mcp`）解释。
- **不用于模型发起的网页抓取**：`web_search` 与 `web_fetch` 必须在只读沙箱内通过外部 curl 与回环代理执行（见 [web](web.md)），不走本模块的进程内客户端。

## 1. HTTP 传输设计（`HttpClient`）

### 1.1 两种接收模式
- **整包接收（`send`）**：读取完整响应体（支持传入绝对截止时间 `deadline` 动态收窄超时），超出最大字节限制时抛出 `too_large` 错误。用于普通 JSON API、决策模型限时审阅、OAuth 认证等。
- **流式接收（`stream`）**：收到完整响应头时首先触发可选的 `on_headers` 回调（供上层如 Streamable HTTP MCP 提前捕获 `Mcp-Session-Id`）；当响应状态码为 `2xx` 时，每收到网络数据块立即在调用线程触发数据回调；非 `2xx` 时不触发流式回调，而是收集上限为 `max_error_body_bytes` 的错误响应体返回（超限时截断并标记 `body_truncated = true`，不抛 `too_large`），确保上层始终能根据状态码与错误体分类处理（如识别 429 限流或上下文超长）。

### 1.2 核心传输策略
1. **状态码与传输错误分离**：收到完整 HTTP 响应（哪怕是 404/429/500）均正常返回 `HttpResponse`，由业务层处理；仅在网络断开、DNS/TLS 失败、超时或取消时抛出分类的 `HttpError`（`cancelled` / `timeout` / `connect` / `tls` / `too_large` / `transport`）。Windows 平台显式开启 `CURLSSLOPT_REVOKE_BEST_EFFORT`，避免内网/离线环境下因无法连接外部证书吊销服务器（CRL/OCSP）而阻断全部 HTTPS 请求。
2. **流式超时控制**：模型推理的流式请求将总超时设为 `0`（不限总时长），仅依赖 `idle_timeout`（默认 300 秒无任何字节到达才判定超时）。这样既能容忍长思考或本地大模型冷启动（期间服务端发送 `: keep-alive` 注释重置空闲计时），又能及时发现连接假死。
3. **即时取消**：跨线程触发 `stop_token` 时，内部通过 `curl_multi_wakeup` 立即唤醒阻塞等待并中止传输。
4. **连接复用与线程模型**：单个 `HttpClient` 实例在调用线程同步执行，跨请求复用 TCP 连接、DNS 缓存与 TLS 会话；多线程并发请求时每个线程持有独立实例。
5. **不自动跟随重定向**：`3xx` 原样返回给上层，防止认证请求头被自动转发到不可信端点。

## 2. SSE 增量解析（`SseParser`）

流式响应的数据块可能在任意字节边界（包括多字节 UTF-8 字符中间或 `\r\n` 之间）被网络层切分。`SseParser` 维护轻量状态机完成增量重组：

```text
网络字节块 (chunk) ──► 缓冲与行切分 (支持 LF/CRLF/CR) ──► 忽略 ':' 注释行与 BOM
                                                        ──► 累加多行 data: 与 event:/id:
                                                        ──► 遇空行派发完整 SseEvent
```

- 仅当遇到空行且包含 `data:` 字段时才向上层派发事件；流结束时未以空行闭合的残缺事件按规范丢弃。
- `data: [DONE]` 等协议特定结束标记不做特殊拦截，原样交由上层模型编解码器识别。
