# 04 net：HTTP、SSE 与 LLM 编解码

库 `dagent_net`，命名空间 `dagent::net`。依赖 libcurl（系统库）。

| 子功能 | 头文件 | 里程碑 | 状态 |
| --- | --- | --- | --- |
| HTTP 客户端 | `net/http.hpp` | — | **已完成** |
| SSE 解析 | `net/sse.hpp` | — | **已完成** |
| LLM 编解码 | 待定 | M3 | **边界模块，归属由你决定** |

---

## 1. 已完成部分

接口见 [http.hpp](../../src/public/net/http.hpp)、[sse.hpp](../../src/public/net/sse.hpp)，要点如下：

- `HttpClient::send` 一次收完整个响应体；`stream` 在状态码为 2xx 时，每到一段数据就在调用线程上回调一次。
- 取消用 `std::stop_token`，底层是 curl multi 加 `curl_multi_wakeup`，发出取消后 0.6ms 内生效（已实测）。
- 超时有三种：总时长、建连、空闲。长流要用空闲超时。
- 传输层失败抛 `HttpError`，按 `cancelled/timeout/connect/tls/too_large/transport` 分类；HTTP 状态码不抛异常，看返回值里的 `status`。
- 同一个实例会复用连接；回调里抛出的异常会原样传回调用方。
- `SseParser` 数据任意切分都能处理，支持 CRLF、CR、BOM，跳过注释行。用真实的 SSE 数据逐字节喂入，结果和整段喂入一致。

剩下的工作只有一项：写 `docs/design/net.md` 设计文档。

---

## 2. LLM 编解码 · M3 · 边界模块

### 归属问题

编解码要在「中立消息模型」和「厂商协议」之间来回翻译，所以它必须知道消息模型的类型。按约定，外围不能
依赖核心，因此有两种放法：

| 方案 | 做法 | 适用情况 |
| --- | --- | --- |
| **A. 放在核心**（推荐） | 消息模型和编解码都在核心，net 只负责 HTTP 和 SSE | 你希望完全掌控消息模型的形状 |
| B. 放在 net | 在 `net/llm.hpp` 里定义中立的消息和事件类型，核心直接使用这些类型 | 你愿意让消息模型成为外围提供的公共类型 |

下面的设计两种方案都适用，差别只在类型定义在哪里。

### 职责

```
中立 Request ──encode──► 厂商 JSON ──HttpClient──► SSE ──decode──► 中立 StreamEvent 序列
```

不做：重试策略、上下文压缩、工具执行（都属于核心）。

不引入厂商 SDK：C++ 没有官方 SDK，社区 SDK 又会把消息模型绑死在它的类型上。

### 建议的中立事件类型

```cpp
struct TextDelta      { std::string text; };
struct ReasoningDelta { std::string text; };                      // qwen/deepseek 的 reasoning_content
struct ToolCallBegin  { int index; std::string id, name; };
struct ToolCallDelta  { int index; std::string args_fragment; };  // 参数 JSON 的片段，不能单独解析
struct ToolCallEnd    { int index; };
struct Usage          { int prompt, completion, cached = 0; };
struct Finish         { enum Reason { stop, length, tool_calls, content_filter, error } reason; std::string raw; };
using StreamEvent = std::variant<TextDelta, ReasoningDelta, ToolCallBegin, ToolCallDelta, ToolCallEnd, Usage, Finish>;

class Codec {                                                     // 每家厂商一个实现
public:
    virtual HttpRequest encode(const Request&) const = 0;
    virtual void decode(const SseEvent&, std::vector<StreamEvent>& out) = 0;   // 有状态：一次请求用一个实例
    virtual Error classify(const HttpResponse&) const = 0;        // 非 2xx 响应：能不能重试、等多久
};
```

厂商实现的优先级：**OpenAI Chat Completions**（本地网关、vLLM、Ollama、DeepSeek 等几乎都兼容这个协议）
→ Anthropic Messages → OpenAI Responses。

### 在本地网关上实测到的行为（2026-09-18，127.0.0.1:10000，qwen3.6-35b-a3b）

| 现象 | 对实现的影响 |
| --- | --- |
| 冷启动大约 190 秒，期间只发送 `: keep-alive` 注释行 | HttpOptions 里设 `timeout=0`、`idle_timeout≈60s`；UI 上要显示「模型加载中」 |
| 思考内容放在非标准字段 `delta.reasoning_content` 里 | decoder 要识别这个字段；`max_tokens` 太小的话会被思考全部用光，`content` 为空，`finish_reason="length"` |
| 请求里加 `stream_options.include_usage=true` 以后，最后一个 chunk 是 `choices: []` 加上 `usage` | decoder 不能假设 `choices[0]` 一定存在 |
| 以 `data: [DONE]` 结束 | 收到 `[DONE]` 就结束，不要把它当 JSON 解析 |
| 请求不存在的模型时返回 404，body 是 `{"error":{"message","type","code"}}` | 由 `classify` 解析 |
| **⚠ 网关会丢掉 `tools` 字段**：带不带工具定义，prompt_tokens 都是 11 | 在这个网关上无法做原生工具调用。要么修改网关配置让它透传 `tools`，要么改用 DeepSeek（`.env.dev` 里已经配了 key），要么在核心里实现「在提示词里描述工具」作为兜底 |

### 实现要点（OpenAI Chat Completions）

- **tool_calls 的分片规则**：第一个分片带 `index`、`id`、`function.name`，后续分片只带 `index` 和 `function.arguments` 的片段。**按 index 累积参数字符串，等到 `finish_reason` 到达时再统一解析**，不要每来一个片段就 parse。同一个 chunk 里可能出现多个 index（并行调用多个工具）。
- **参数 JSON 可能是坏的**（小模型经常这样）：解析失败时，把原始字符串交给核心，由核心决定是回给模型让它重试，还是直接报错。decoder 本身不抛异常。
- **流中途出错**：有些网关会在 200 响应的流里发一条 `data: {"error": …}`，要识别出来，转成 `Finish{error}`。
- **429 和 5xx**：读取 `retry-after` 响应头（可能是秒数，也可能是 HTTP 日期），放进 `classify` 的结果里。400 通常意味着上下文超长，要单独标出来，方便核心据此触发压缩。
- **DeepSeek 的特殊之处**：思考模型同样通过 `reasoning_content` 返回思考内容。历史里的 `reasoning_content` 要不要回传，DeepSeek 不同版本的规定不一样：早期版本回传会报 400，后来的版本在工具调用的中间轮次要求回传。**实现前先查当前的官方文档**，并把「是否回传」做成编解码器的一个选项；`usage` 里还有 `prompt_cache_hit_tokens`，映射到 `Usage::cached`。
- **token 估算**：以 API 返回的 `usage` 为准。发请求之前用启发式规则预估：ASCII 大约 4 个字符算 1 个 token，CJK 大约 1 个字符算 1 个 token；再用上一次的 `usage.prompt_tokens` 和估算值的比例做校正。不引入 tokenizer 库。

### 验收（temp/llm_check，对着真实服务跑）

1. 纯文本对话：TextDelta 拼起来的内容，和同一个请求在非流式模式下的 `content` 一致（temperature 设为 0）。
2. 思考模型：ReasoningDelta 和 TextDelta 能分开拿到；`max_tokens` 很小时返回 `Finish{length}`。
3. 工具调用（用 DeepSeek，或者等本地网关修好）：让模型并行调用两个工具，两个 ToolCall 的参数都能完整解析；把工具结果回填进去继续对话，模型能接着往下说。
4. 所有情况下都能拿到 `usage`。
5. 用错误的模型名、错误的 key 请求时，`classify` 分别给出「不可重试」和对应的错误信息，key 不会出现在错误信息里。

### 审核关注点

tool_calls 的累积逻辑；`choices` 为空时的处理；decoder 有没有把状态泄漏到下一次请求。
