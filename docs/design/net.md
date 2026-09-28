# net：HTTP 客户端与 SSE 解析

与远端服务通信的模块。头文件在 `src/public/net/`，实现在 `src/private/net/`，构建为静态库 `dagent_net`，
命名空间 `dagent::net`。唯一的外部依赖是系统的 libcurl（私有链接）。

本模块只负责 HTTP 和 SSE，**不包含任何模型协议**：请求体怎么组织、SSE 事件里的 JSON 怎么解释，都由调用方决定。

---

## 1. 概述

| 能力 | 头文件 | 主要接口 |
| --- | --- | --- |
| HTTP 请求：整包接收或流式接收，可以即时取消，超时分三种，传输错误有分类 | `net/http.hpp` | `HttpClient`、`HttpRequest`、`HttpResponse`、`HttpError` |
| Server-Sent Events 增量解析 | `net/sse.hpp` | `SseParser`、`SseEvent` |

典型用法：流式调用模型接口。

```cpp
net::HttpClient http({.timeout = 0s, .idle_timeout = 60s});
net::SseParser sse;
net::HttpResponse resp = http.stream(
    {"POST", base_url + "/chat/completions", {{"Content-Type", "application/json"}}, body},
    [&](std::string_view chunk) {
        sse.feed(chunk, [&](const net::SseEvent& e) { /* 解析 e.data，再用 Runtime::post 推给界面 */ });
    },
    stop_token);                          // UI 线程按 Esc 时调用 stop_source.request_stop()
if (resp.status != 200) { /* resp.body 是错误体 */ }
```

---

## 2. HttpClient

### 两种接收方式

| 方法 | 行为 |
| --- | --- |
| `send(req, stop)` | 读取完整的响应体，放进 `HttpResponse::body`。超过 `max_body_bytes` 时抛 `HttpError{too_large}` |
| `stream(req, on_data, stop)` | 状态码为 2xx 时，每收到一段数据就调用一次 `on_data`，数据的切分位置是任意的；状态码不是 2xx 时不调用回调，错误体放进返回值的 `body`，超过 `max_error_body_bytes` 的部分截掉 |

两者都会返回状态码和响应头。**HTTP 状态码不是错误**：404、429、500 都正常返回，由调用方看 `status` 自行处理。

### 请求

- `method` 是任意方法名，默认 `POST`；`GET` 不发送请求体；`POST` 会发送 `body`（空的也发送）。其他方法有 `body` 就发送。
- `headers` 原样发送，不需要自己写 `Content-Length`。客户端会禁用 `Expect: 100-continue`，大请求体不再等服务器的 100 响应。
- User-Agent 是 `DAgent/0.1`；开启 TCP keepalive。
- 代理：遵循 libcurl 的默认行为，读取 `http_proxy`、`https_proxy`、`no_proxy` 这几个环境变量。

### 响应

- `headers` 里的名字已经转成小写，重复的头都保留，顺序和收到的一致。经过 1xx 中间响应时，只保留最终响应的头。
- `header("retry-after")` 返回第一个同名头的值，参数必须是小写。

### 选项：HttpOptions

对应 `home/config/config.json` 的 `"http"` 段。时长为 0 表示不限制。

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `timeout` | 300s | 整个请求的总时长上限 |
| `connect_timeout` | 30s | 建连（含 TLS 握手）上限；为 0 时用 libcurl 自己的默认值 300 秒 |
| `idle_timeout` | 0 | 连续这么久收不到任何字节就判为超时 |
| `max_body_bytes` | 2 MiB | `send()` 响应体的上限 |
| `max_error_body_bytes` | 4 KiB | `stream()` 保留错误体的最大长度 |
| `verify_peer` / `verify_host` | true | TLS 证书校验 |

**长时间的流式请求**要设 `timeout = 0`，只用 `idle_timeout` 来判断卡死。比如本地懒加载的模型冷启动要 3 分钟，
期间服务器只会发送 `: keep-alive` 注释，这些字节会不断重置空闲计时，所以不会被误判成超时。

### 错误：HttpError

只有没拿到完整的 HTTP 响应时才抛异常，`kind()` 表示失败的类型：

| Kind | 含义 | 调用方一般怎么处理 |
| --- | --- | --- |
| `cancelled` | stop_token 请求了停止 | 不重试 |
| `timeout` | 总时长或空闲超时 | 可以重试 |
| `connect` | 域名解析失败或连接不上 | 可以重试，也可能是配置写错了 |
| `tls` | 证书或握手失败 | 不重试 |
| `too_large` | 响应体超过 `max_body_bytes` | 不重试 |
| `transport` | 连接中途断开等其他错误 | 通常可以重试 |

错误信息的格式是 `方法 URL 失败：libcurl 的描述`，**不包含请求头和请求体**。但完整的 URL 会出现在信息里，
所以密钥不要放在 URL 的查询参数里，要用请求头传递。

### 取消

`stop_token` 被触发时，客户端会立即唤醒内部的 `curl_multi_poll`，不需要等轮询周期，实测 0.6ms 以内就会
抛出 `cancelled`。取消请求要从其他线程对对应的 `stop_source` 调用 `request_stop()`。

### 线程与连接复用

- 请求在调用线程上阻塞执行，`on_data` 也在调用线程上触发。
- **同一个实例不能在多个线程上并发使用**；需要并发请求时，每个线程各自持有一个 `HttpClient`。
- 同一个实例上的请求串行执行，连接、DNS 缓存和 TLS 会话在请求之间保留，连续调用模型时不用每次重新握手。
- `on_data` 抛出的异常会中止本次传输，并在 `stream()` 返回前原样重新抛出。
- libcurl 的全局初始化由第一个构造的 `HttpClient` 完成，只执行一次，并且是线程安全的。

---

## 3. SseParser

按 WHATWG 的 Server-Sent Events 规范做增量解析：

```cpp
net::SseParser parser;                                 // 一次响应用一个实例
parser.feed(chunk, [](const net::SseEvent& e) { … });  // 每解析出一个完整事件调用一次
```

- 数据**任意切分都能处理**，包括在多字节字符中间、在 `\r\n` 两个字节之间切开的情况。实测用真实的 SSE 数据逐字节喂入，结果和整段喂入完全一致。
- 行尾可以是 LF、CRLF 或单独的 CR；流开头的 BOM 会被去掉。
- 以 `:` 开头的注释行（比如 `: keep-alive`）直接跳过。
- 遇到空行时派发一个事件；多行 `data:` 用 `\n` 连起来。只有 `event:`、没有 `data:` 的事件不会派发。
- `SseEvent::event` 在没有 `event:` 字段时为空，按规范等同于 `"message"`；`id` 跨事件保留最近一次的值。`retry:` 字段会被忽略。
- 流结束时，没有以空行结尾的最后一个事件按规范丢弃。
- `data: [DONE]` 这类结束标记属于模型协议，解析器不做特殊处理，由调用方识别。

---

## 4. 依赖与构建

- libcurl：`find_package(CURL)`，私有链接 `CURL::libcurl`。本机的版本是 8.5.0，`curl_multi_wakeup` 需要 7.68 及以上。
