# LLM 编解码：消息模型与厂商协议翻译

核心 agent 的一部分（不是外围模块），头文件在 `src/public/agent/`，实现在 `src/private/agent/`，构建为
静态库 `dagent_agent`，命名空间 `dagent::agent`。依赖 base 和 net（`net::HttpClient` 发请求、
`net::SseParser` 拆流）。

```
中立 Request ──encode──► 厂商 JSON ──HttpClient──► SSE ──decode──► 中立 StreamEvent 序列
```

不做：重试策略、上下文压缩、工具执行——这些都是核心里更上层的部分，用这里产出的中立事件做决策。
不引入厂商 SDK：C++ 没有官方 SDK，社区 SDK 会把消息模型绑死在它的类型上。

---

## 1. 概述

| 内容 | 头文件 | 主要类型/接口 |
| --- | --- | --- |
| 中立消息模型 | `agent/message.hpp` | `Role`、`Message`、`ToolCall`、`ToolDef`、`Request` |
| 中立流事件、错误分类、Codec 接口、token 估算 | `agent/llm.hpp` | `StreamEvent`、`Error`、`Codec`、`estimate_tokens`、`TokenEstimator` |
| OpenAI Chat Completions 编解码器 | `agent/openai_chat.hpp` | `OpenAiChatOptions`、`make_openai_chat_codec` |

厂商实现的优先级：**OpenAI Chat Completions**（本地网关、vLLM、Ollama、DeepSeek 等几乎都兼容这个协议）
→ Anthropic Messages → OpenAI Responses。目前只写了第一个：本机没有可实测的 Anthropic/Responses 服务，
不写没有真实验证过的代码；`Codec` 是纯虚接口，之后加实现不影响已有调用方。

---

## 2. 中立消息模型（message.hpp）

```cpp
enum class Role { system, user, assistant, tool };

struct ToolCall { std::string id, name, arguments; };  // arguments 可能是坏的 JSON，由核心决定怎么处理

struct Message {
    Role role = Role::user;
    std::string content;
    std::string reasoning_content;    // assistant 的思考内容；是否回传由 codec 选项决定
    std::vector<ToolCall> tool_calls; // role == assistant 时有效
    std::string tool_call_id;         // role == tool 时有效
};

struct ToolDef { std::string name, description; nlohmann::json parameters; };  // JSON Schema

struct Request {
    std::string model;
    std::vector<Message> messages;
    std::vector<ToolDef> tools;
    std::size_t max_tokens = 0;  // 0 表示不发送，交给服务端默认值
    double temperature = -1.0;   // < 0 表示不发送
    bool stream = true;
};
```

这套类型和具体厂商无关，编解码器负责翻译成各自的 JSON；消息历史（要不要回传 `reasoning_content`、
压缩策略）由核心管理，这里只是纯数据。

---

## 3. StreamEvent 与 Codec（llm.hpp）

```cpp
using StreamEvent = std::variant<TextDelta, ReasoningDelta, ToolCallBegin, ToolCallDelta, ToolCallEnd,
                                 Usage, Finish>;

class Codec {
public:
    virtual net::HttpRequest encode(const Request&) const = 0;
    virtual void decode(const net::SseEvent&, std::vector<StreamEvent>& out) = 0;
    virtual Error classify(const net::HttpResponse&) const = 0;
};
```

- **`decode` 有状态，一次请求用一个实例**：内部要按 `index` 累积工具调用参数的分片，同一个实例不能跨请求
  复用。`Finish` 只在协议的结束标记（OpenAI 是 `data: [DONE]`）出现时发出；发出之后再喂数据不产生任何
  事件。`Usage` 保证出现在 `Finish` 之前（如果服务端给了的话）。
- **`Finish::Reason`**：`stop`/`length`/`tool_calls`/`content_filter` 是正常结束；`finish_reason`
  缺失或者是编解码器不认识的字符串，一律映射成 `error`——不能把截断误判成正常结束。
- **`Error`**：`classify` 只依据 HTTP 状态码和响应体判断，不抛异常；`retry_after` 为 0 表示服务端没给，
  不代表「立即重试」，由核心自己决定退避策略；`message` 保证不含密钥。
- **token 估算**：`estimate_tokens` 是启发式规则（ASCII 约 4 字节 1 token，非 ASCII 码点约 1 个 1
  token），`TokenEstimator` 用上一次真实 `usage.prompt_tokens` 与估算值的比例做指数滑动平均校正
  （新系数 = 0.5×旧系数 + 0.5×本次比例）。不引入 tokenizer 库。`estimate_prompt_tokens` 不统计历史消息里
  的 `reasoning_content`（要不要回传是 codec 选项决定的运行时行为，静态估算拿不到），这部分偏差由校正
  系数吸收。

---

## 4. OpenAI Chat Completions 编解码器（openai_chat.hpp/.cpp）

```cpp
struct OpenAiChatOptions {
    std::string base_url, api_key;      // api_key 从 base::Secrets 读出后传入，不写日志
    bool send_reasoning_content = false; // 历史里的 reasoning_content 要不要回传
    bool include_usage = true;           // stream_options.include_usage；个别网关不支持时关掉
    nlohmann::json extra_body;            // 透传网关专属字段，不覆盖已有字段
};
std::unique_ptr<Codec> make_openai_chat_codec(OpenAiChatOptions);
```

### encode

- URL 是 `{base_url}/chat/completions`（去掉 `base_url` 结尾多余的斜杠再拼）。
- `tool_calls` 非空但 `content` 为空时，`content` 发 JSON `null`（不是空字符串）——部分网关按这个区分
  「纯工具调用」和「带说明的工具调用」。
- `stream=true` 且 `include_usage` 时加 `stream_options.include_usage=true`；`stream=false` 时不发
  `stream_options`。
- `extra_body` 逐个 key 合并进请求体，**已经由 `Request` 生成的字段不会被覆盖**（先 `contains` 检查）。
- 没设置的 `max_tokens`（0）、`temperature`（< 0）不写进请求体，交给服务端默认值。

### decode：分片累积规则

- **工具调用**：分片按 `index` 累积。第一次出现某个 `index` 时（`fragment.contains("index")`，缺失时
  按出现顺序补一个）发 `ToolCallBegin`（带 `id`、`function.name`）；后续分片只带 `function.arguments`
  的片段，追加成 `ToolCallDelta`。**不在每个分片到达时解析 JSON**，原始片段原样交给核心，等
  `[DONE]` 时按首次出现的顺序统一补 `ToolCallEnd`。同一个 chunk 里可以出现多个 `index`（并行调用）。
- **`choices` 可能是空数组**：开了 `include_usage` 之后，最后一个 chunk 形如 `{"choices": [], "usage":
  {...}}`，decode 遇到空 `choices` 直接返回（不当错误），但已经先处理过 `usage` 字段。
- **流中途的错误**：chunk 里出现非 null 的 `error` 字段时，转成 `Finish{error}` 并关闭这个实例，不再
  处理后面的数据。
- **坏 JSON**：`json::parse(..., allow_exceptions=false)` 失败时同样转成 `Finish{error}`，decoder 本身
  不抛异常。
- **usage 字段**：`prompt_tokens`/`completion_tokens` 直接取；`cached` 优先取 DeepSeek 的
  `prompt_cache_hit_tokens`，没有的话退回 OpenAI 的 `prompt_tokens_details.cached_tokens`。

### classify

- 429、408 视为可重试；5xx 可重试；其余（包括 400、401）不可重试。
- `Retry-After` 响应头：纯数字按秒解析；否则按 HTTP-date（`Wed, 21 Oct 2015 07:28:00 GMT`）手工解析，不
  依赖 locale。header 缺失或解析失败时 `retry_after` 为 0。
- 400 且响应体里出现「context length」「too many tokens」等几个英文短语（大小写不敏感）时标记
  `context_too_long = true`，供核心决定要不要触发上下文压缩后重试。这是关键词启发式，不追求完全覆盖
  所有厂商的措辞。
- 错误信息优先取响应体 `error.message`（`error.code` 不同于 message 时附在后面），没有就截取原始 body
  的前 300 字节；两种情况都不会包含请求里的 `api_key`（key 只在 `encode` 时写进 `Authorization` 头，
  `classify` 只读响应体和响应头）。

### DeepSeek 的特殊之处

历史里的 `reasoning_content` 要不要回传，DeepSeek 不同版本要求不一样（早期版本回传会报 400，后来的
版本在工具调用的中间轮次要求回传），做成了 `send_reasoning_content` 选项，而不是写死的行为。

---

## 5. 在本地网关上的实测行为（127.0.0.1:10000，qwen3.6-35b-a3b，OpenAI 兼容）

- 冷启动大约 190 秒，期间只发送 `: keep-alive` 注释行（SSE 注释，`net::SseParser` 已经会跳过，不会被
  当成事件）。
- 思考内容放在非标准字段 `delta.reasoning_content` 里，和 DeepSeek 一致，用同一套 decode 逻辑处理。
- **这个网关会丢掉请求里的 `tools` 字段**：带不带工具定义，`prompt_tokens` 都不变，也就没法在这个网关上
  测原生工具调用（验收 3 用 DeepSeek 做的）。`extra_body` 选项就是为了给这一类网关专属参数（比如这个
  网关的 `enable_thinking`）留一个出口，同时保证不覆盖 `Request` 已经生成的核心字段。
- 请求不存在的模型时返回 404，body 是 `{"error":{"message","type","code"}}`，走 `classify` 的默认分支
  （非 2xx 且不在可重试状态码里）。

---

## 6. 依赖与构建

- 只依赖 base（token 估算用不到任何三方库）和 net（`HttpRequest`/`HttpResponse`/`SseEvent`）。
- 需要真实请求的检测用 DeepSeek；本地网关的实测行为见 §5。
