# 01 内部统一格式

> 里程碑 P1 · 文件 `src/public/agent/message.hpp`、`llm.hpp`

核心只认这套结构，任何一家的字段名都不许出现在它里面。Provider 负责两个方向的翻译：
**请求：内部 → 外部**，**流与错误：外部 → 内部**。

---

## 1. 现状

已经中立，不用动的部分：

```cpp
struct Message {   // message.hpp
    Role role;                        // system / user / assistant / tool
    std::string content;
    std::string reasoning_content;    // assistant 的思考
    std::vector<ToolCall> tool_calls; // {id, name, arguments(JSON 文本)}
    std::string tool_call_id;         // role == tool 时回填给哪次调用
};
struct Request { model, messages, tools, max_tokens, temperature, stream };
using StreamEvent = variant<TextDelta, ReasoningDelta, ToolCallBegin,
                            ToolCallDelta, ToolCallEnd, Usage, Finish>;
struct Error { retryable, context_too_long, retry_after, message };
```

这套结构已经能表达 Anthropic 的请求：assistant 的 `content` + `tool_calls` 对应它的
`[text, tool_use]` 内容块，`role == tool` + `tool_call_id` 对应它的 `user` 消息里的 `tool_result` 块。
**翻译在 Provider 里做，内部不引入「内容块」概念**——内容块是 Anthropic 的表达方式，不是我们的。

## 2. 要补的三处

### 2.1 思考的可回传性

现在 `reasoning_content` 只有文本。Anthropic 打开 extended thinking 后，带工具调用的多轮里要求把
thinking 块连同 `signature` 一起回传，否则报错。补一个字段，其它家忽略它：

```cpp
struct Message {
    …
    std::string reasoning_signature; ///< 厂商要求回传思考时的校验串；没有就留空
};
```

**P1 只加字段，不打开 Anthropic 的 thinking**（[05 §2](05-adapters.md)）。先让不带思考的路径跑通。

### 2.2 停止原因要够用

`Finish::Reason` 现在是 `stop / length / tool_calls / content_filter / error`。
Anthropic 的 `refusal`、Ollama 的 `done_reason: load`/`unload` 都要能落位：
`refusal` 归到 `content_filter`，不认识的一律 `error` 并把原文留在 `Finish::raw`（字段已有）。
不新增枚举值——核心对 finish 的处理只分这几类，多加会让上层多写分支。

### 2.3 上下文窗口跟着模型走

`context.window_tokens` 现在是全局一个值。不同模型窗口差很多，切模型后预算必须跟着变：
`ProviderConfig::context_window`（[02](02-provider.md)），为 0 时回落到 `context.window_tokens`，
Agent 取它做预算与 `ContextUpdate`。

**不做自动探测**，窗口只来自配置。理由是探了也覆盖不全：OpenAI、Anthropic、DeepSeek、智谱的
`/v1/models` 都只给 id，没有窗口字段；Ollama 的 `/api/show` 给的是模型上限，而实际生效的是请求里的
`options.num_ctx`，探到的数字反而会误导。为少数几家写探测分支，换来的是每家一条要维护的代码路径。

配错了有现成的安全网：服务端说超长时 `classify` 判成 `context_too_long`，Agent 强制压缩后重发一次。
这条现在就在工作。

## 3. 映射表

Provider 写代码时按这张表对齐，别各写各的：

| 内部 | openai-chat | anthropic | ollama 原生 |
| --- | --- | --- | --- |
| `Role::system` 消息 | `messages[0].role="system"` | 顶层 `system` 字符串 | `messages[0].role="system"` |
| `Message.content` | `content` | `content:[{type:"text"}]` | `message.content` |
| `Message.tool_calls` | `tool_calls[].function{name,arguments}` | `content:[{type:"tool_use",id,name,input}]` | `message.tool_calls[].function` |
| `Role::tool` + `tool_call_id` | `role="tool", tool_call_id` | `role="user"`，块 `{type:"tool_result",tool_use_id}` | `role="tool"`（无 id，按顺序配对） |
| `reasoning_content` | `reasoning_content`（部分网关叫 `reasoning`） | `content:[{type:"thinking",signature}]` | `message.thinking` |
| `ToolDef` | `tools[].function{name,description,parameters}` | `tools[]{name,description,input_schema}` | `tools[].function` |
| `max_tokens` | 可省 | **必填** | `options.num_predict` |
| `TextDelta` | `delta.content` | `content_block_delta.text_delta` | 每行 `message.content` |
| `ToolCallBegin/Delta` | `delta.tool_calls[]` 带 index | `content_block_start(tool_use)` + `input_json_delta` | 整包给出，无增量 |
| `Usage` | `usage`（要 `stream_options.include_usage`） | `message_start` + `message_delta` 的 usage 相加 | `prompt_eval_count` / `eval_count` |
| `Finish` | `finish_reason` | `message_delta.stop_reason` | `done_reason` |

**ollama 原生没有工具调用 id**：Provider 自己生成（`call_0`、`call_1`…）并在本次请求内记住顺序，
回填时按这个映射还原。这是 Provider 的内部事，内部格式不知情。

## 4. 完成标准

1. `message.hpp` 与 `llm.hpp` 里没有任何厂商字段名与 URL。
2. 加了 `reasoning_signature` 后，现有 openai-chat 路径行为不变（本地模型跑一轮确认）。
3. 会话记录的读写覆盖新字段，旧记录回放不受影响（`record.cpp` 缺字段按空处理）。
