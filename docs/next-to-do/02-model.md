# 02 Model：一次模型调用

> 里程碑：C1 · 头文件 `agent/model.hpp` · 实现 `model.cpp` · 依赖 net、llm（Codec）

把「中立 Request → 流式 HTTP → 中立 Reply」包成一个阻塞调用，负责流的累积、失败分类和重试。Agent 的每一步和
压缩时的摘要请求都走它。

---

## 1. 职责

**做**：新建 Codec、发请求、喂 SSE、把 StreamEvent 边转发边累积成 Reply；判断失败能不能重试、等多久；重试时通知
调用方丢弃已显示的部分；可取消的等待。

**不做**：决定请求里放什么（Conversation）、上下文超长后怎么压缩（[07-context](07-context.md)）、解析工具参数
（坏 JSON 交给 `tool.prepare` 报错，模型看得到，能自己修）。

---

## 2. 接口

```cpp
namespace dagent::agent {

struct ModelParams {                 ///< 来自 gateway 段，每次 build Request 时用
    std::string model;
    std::size_t max_tokens = 0;
    double temperature = -1.0;
};

struct RetryOptions {
    int max_retries = 2;                                  ///< run.max_model_retries；总尝试次数 = 1 + max_retries
    std::chrono::milliseconds base_delay{1000};
    std::chrono::milliseconds max_delay{30000};
    std::chrono::milliseconds max_retry_after{300000};    ///< 服务端要求等更久就不等了
};

struct Reply {
    Message message;                 ///< role = assistant；content、reasoning_content、tool_calls
    Finish finish;
    std::optional<Usage> usage;
};

struct RetryInfo {
    int attempt, max_attempts;       ///< 第几次重试（从 1 开始）、最多几次
    std::chrono::milliseconds wait;
    std::string reason;              ///< 给人看：「HTTP 503」「连接中断」「流没有正常结束」
    bool had_output;                 ///< 这次失败的尝试是否已经交出过事件（决定要不要 StreamReset）
};

class ModelError : public std::runtime_error {
public:
    enum class Kind {
        cancelled,        ///< stop_token；partial() 是这次尝试已收到的内容
        context_too_long, ///< Codec::classify 判定上下文超长
        rejected,         ///< 不可重试：401/403/400/404、TLS、响应过大、网关不返回 SSE
        exhausted,        ///< 可重试的错误用完了重试次数，或 Retry-After 太长
    };
    Kind kind() const noexcept;
    const Reply& partial() const noexcept;
};

class Model {
public:
    Model(std::function<std::unique_ptr<Codec>()> codec_factory, net::HttpOptions http, RetryOptions retry);

    /// 阻塞到拿到完整回复。on_event 在调用线程上收到每个 StreamEvent（含 Usage、Finish）；
    /// on_retry 在每次重试等待之前调用。
    Reply complete(const Request& request,
                   const std::function<void(const StreamEvent&)>& on_event,
                   const std::function<void(const RetryInfo&)>& on_retry,
                   std::stop_token stop);
};

} // namespace dagent::agent
```

---

## 3. 一次尝试

```
codec = codec_factory()                            # 有状态，一次尝试一个，不能复用
http.stream(codec.encode(request), on_data, stop)
  on_data(chunk): sse.feed(chunk, [](event) { codec.decode(event, events); 逐个 accumulate + on_event })
检查结果（§4）
```

### 3.1 累积规则

| StreamEvent | 累积到 Reply |
| --- | --- |
| `TextDelta` | `message.content += text` |
| `ReasoningDelta` | `message.reasoning_content += text` |
| `ToolCallBegin{index, id, name}` | 按 index 新建一个 ToolCall，记下它**第一次出现的顺序** |
| `ToolCallDelta{index, fragment}` | 追加到该 index 的 `arguments` |
| `ToolCallEnd{index}` | 标记完成（Codec 在 `[DONE]` 时统一发） |
| `Usage` | `usage = …` |
| `Finish` | `finish = …`，这次尝试结束 |

- `message.tool_calls` 按第一次出现的顺序排列，不按 index 数值排序（index 可能不连续）。
- **id 为空的工具调用补一个 id**：`call_<step>_<序号>`。个别网关不给 id，而 tool 消息必须能对上。补的 id 在一个会话
  内唯一即可。
- **同一条回复里重复的 id**：第二个起改成 `<id>_<序号>`。OpenAI 协议下重复 id 会让下一次请求 400。
- 不在这里解析参数 JSON。

### 3.2 转发规则

`on_event` 收到每一个 StreamEvent，Agent 据此转成对外事件（`TextDelta` → `TextDelta`，`ToolCallBegin` →
`ToolPending`，`Usage`、`Finish`、`ToolCallDelta`、`ToolCallEnd` 不外发）。**是否已经交出过可见事件**
（Text / Reasoning / ToolCallBegin）要记下来，决定重试时是否需要 `StreamReset`。

---

## 4. 失败分类

一次尝试结束后，按下表判断：

| 情况 | 结果 |
| --- | --- |
| `HttpError::cancelled` | 抛 `cancelled`，`partial()` 是这次尝试已累积的 Reply |
| `HttpError::connect` / `transport` / `timeout` | 可重试 |
| `HttpError::tls` | 抛 `rejected`（证书问题，重试无意义） |
| `HttpError::too_large` | 抛 `rejected`（只有非流式才会遇到，这里理论上不出现） |
| 状态码非 2xx，`classify` 的 `context_too_long` | 抛 `context_too_long`（优先于 retryable 判断） |
| 状态码非 2xx，`classify` 的 `retryable` | 可重试，等待 `max(retry_after, 退避)` |
| 状态码非 2xx，其他 | 抛 `rejected`，信息 = `Error::message`（保证不含密钥） |
| 2xx，**一个 SSE 事件都没有** | 抛 `rejected`：「网关没有返回 SSE 流，可能不支持 stream=true」 |
| 2xx，收到 `Finish{error}` | 可重试（流中途 error 字段、坏 JSON、缺 finish_reason） |
| 2xx，流结束了却没有 `Finish` | 可重试：连接关了却没有 `[DONE]`，不能把截断当成正常结束 |
| 2xx，收到正常的 `Finish` | 成功，返回 Reply |

`cancelled` 永远优先：stop 已请求时，无论底层报什么错，都按 `cancelled` 处理。

---

## 5. 重试

```
for attempt in 0 .. max_retries:
    try 一次尝试 → 成功就 return
    不可重试 → throw
    if attempt == max_retries → throw exhausted（信息带最后一次的原因）
    wait = backoff(attempt + 1)，服务端给了 retry_after 时 wait = max(wait, retry_after)
    if retry_after > max_retry_after → throw exhausted（「服务端要求 N 分钟后重试」）
    on_retry(RetryInfo{attempt + 1, max_retries, wait, reason, had_output})
    可取消地等 wait；等待中 stop → throw cancelled（partial 为空）
```

- 退避：`backoff(n) = min(max_delay, base_delay × 2^(n-1)) × U(0.8, 1.2)`。默认值下依次约为 1 s、2 s、4 s……
- 可取消的等待：

  ```cpp
  std::mutex m;
  std::condition_variable_any cv;
  std::unique_lock lock(m);
  cv.wait_for(lock, stop, wait, [] { return false; });   // stop 请求时立即返回
  if (stop.stop_requested()) throw ModelError(cancelled, …);
  ```

  不要用 `sleep_for`：用户按 Esc 后最多要等 30 秒。
- 重试是**整个请求重发**，不做续传。`had_output` 为 true 时，Agent 在收到 `on_retry` 后发 `Retrying` + `StreamReset`。
- 每次重试记一条 warn：原因、状态码、等待时长、第几次。不记请求体。

---

## 6. HTTP 选项

`http` 段的 `timeout_seconds`（默认 300 秒）是**整个请求**的上限，一个长回复就会被它切断。Model 用的 HttpOptions：

| 字段 | 取值 |
| --- | --- |
| `timeout` | **0（不限）** |
| `idle_timeout` | `http.idle_timeout_seconds`；新配置默认 120 秒 |
| `connect_timeout`、`verify_*`、`max_error_body_bytes` | 照用配置 |

本地网关冷启动（约 190 秒）期间只发 `: keep-alive` 注释行；注释行也是收到的字节，不会触发 `idle_timeout`。

`net::HttpClient` 由 Model 持有，在 agent 线程上复用（连接、DNS、TLS 会话保留）。不跨线程使用。

---

## 7. Codec 与参数的来源

`codec_factory` 在装配时（[11-entry](11-entry.md)）按 gateway 配置做好：

| `OpenAiChatOptions` | 来源 |
| --- | --- |
| `base_url` | `gateway.base_url` |
| `api_key` | `gateway.api_key`（app 已从 Secrets 取出；本地网关可以为空） |
| `send_reasoning_content` | 新配置 `gateway.send_reasoning_content`，默认 false（DeepSeek 思考模式的工具轮次要开） |
| `include_usage` | 新配置 `gateway.include_usage`，默认 true |
| `extra_body` | 新配置 `gateway.extra_body`，对象原样透传；现有的 `gateway.enable_thinking` 删掉，改成在 `extra_body` 里写 `"enable_thinking": false` |

`ModelParams` 来自 `gateway.model`、`gateway.max_tokens`、`gateway.temperature`（`std::optional` 为空 → `-1.0`）。

---

## 8. 边界情况

| 情况 | 处理 |
| --- | --- |
| 带着 tool_calls 却报 `finish_reason: stop` | Model 不管，照常返回；Agent 只看 tool_calls 是否为空 |
| 只有思考内容、没有正文也没有调用 | 照常返回；Agent 按空回复处理（[04-turn §4](04-turn.md)） |
| 网关不返回 usage | `usage` 为空；估算照旧，状态栏显示估算值 |
| 429 带 `Retry-After: 2` | 等 `max(2 s, 退避)` |
| 429 带 `Retry-After: 3600` | 超过 `max_retry_after`，直接 `exhausted`，提示几分钟后再试 |
| 取消发生在建连阶段 | net 保证即时返回 `cancelled` |
| 取消后下一轮复用 HttpClient | 被取消的连接由 net 丢弃，下一次请求重新建连，无需特殊处理 |
| 流到一半断开，已经输出了半句话 | `Retrying`（`had_output = true`）→ `StreamReset` → 整个重发 |

---

## 9. 实现要点

- 一次尝试的状态（Codec、SseParser、累积中的 Reply、`had_output`）放在一个局部结构里，每次尝试新建，避免上一次
  尝试的残留混进来。
- `on_data` 里调 `on_event` 时如果 Sink 抛异常，net 会中止传输并原样抛出（net 文档），Model 不 catch，让它传出去。
- 把「分类」写成一个纯函数 `classify_attempt(结果) → {成功 / 可重试(原因, retry_after) / 抛出(kind, 信息)}`，重试循环
  只管次数和等待。

---

## 10. 验收

AGENTS.md 不允许模拟模型，所以不写假网关，而是写一个临时的**故障注入代理**（放在 `temp/` 下）：把请求原样转发给
真实网关（DeepSeek），回复内容都是真的，代理只在网络层注入故障。`gateway.base_url` 指向代理，代理的上游和故障模式
用命令行参数指定：

| 模式 | 代理的行为 | 期望 |
| --- | --- | --- |
| `pass` | 原样转发 | 成功 |
| `503x2` | 前两个请求直接回 503，之后转发 | 两次 `Retrying`，成功 |
| `cut` | 第一个请求转发到一半时断开连接，之后转发 | `Retrying` + `StreamReset`，成功 |
| `nodone` | 第一个请求吞掉 `data: [DONE]` 后正常关闭连接 | 按「流没有正常结束」重试，而不是当成正常结束 |
| `retry-after` | 第一个请求回 429 + `Retry-After: 2` | 等待不少于 2 秒后成功 |
| `slow` | 每个 SSE 事件延迟 2 秒转发 | 不被 `http.timeout_seconds` 切断（把它临时设成 10 验证）；中途 Ctrl+C 立即停 |

另外两个不需要代理的场景：

- 错误的 api key 直连 DeepSeek → 401，立即失败（`rejected`），退出码 1，输出里没有 key；
- `base_url` 指向一个没人监听的端口 → 连接失败，重试用完后 `exhausted`。

对应 plan 场景 3、4。
