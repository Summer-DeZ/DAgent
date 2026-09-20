# 05 各家适配

> 里程碑 P5 · `src/private/agent/provider_*.cpp`

一家一个文件，每家只做三件事：`encode`、`decode`、`classify`。映射规则见 [01 §3](01-internal.md)。

---

## 1. openai-chat（已有，只做搬家）

现有 `openai_chat.cpp` 改成 provider 之一，顺手补两处：

- **思考字段兼容**：除 `delta.reasoning_content` 外，再认 `delta.reasoning`（OpenRouter、部分 vLLM 构建）。
  两个都出现时以 `reasoning_content` 为准。
- **`max_tokens` 的新名字**：OpenAI 新模型要求 `max_completion_tokens`。
  不做自动探测，由配置的 `extra_body` 覆盖——这是「配置声明而非探测」原则的一次应用。

覆盖的服务：OpenAI、DeepSeek、智谱、Qwen、OpenRouter、llama.cpp、vLLM、LM Studio、Ollama 的 `/v1`。

## 2. anthropic（新写）

| 项 | 值 |
| --- | --- |
| 端点 | `POST {base_url}/messages`，默认 `https://api.anthropic.com/v1` |
| 头 | `x-api-key: <key>`、`anthropic-version: 2023-06-01`、`content-type: application/json` |
| 必填 | `max_tokens`（`needs_max_tokens = true`） |
| system | 顶层 `system` 字符串，不放进 `messages` |
| 工具 | `tools[]{name, description, input_schema}`；调用是 `tool_use` 内容块 |
| 工具结果 | `role:"user"` 的消息里放 `{type:"tool_result", tool_use_id, content}` |
| 流 | SSE，带 `event:` 类型 |

**流事件映射**：

| SSE 事件 | 动作 |
| --- | --- |
| `message_start` | 记 `usage.input_tokens`（缓存读写也在这里） |
| `content_block_start` | `text` → 无；`tool_use` → `ToolCallBegin{index, id, name}`；`thinking` → 无 |
| `content_block_delta` | `text_delta` → `TextDelta`；`input_json_delta` → `ToolCallDelta{partial_json}`；`thinking_delta` → `ReasoningDelta`；`signature_delta` → 存进 `reasoning_signature` |
| `content_block_stop` | `tool_use` 块结束时发 `ToolCallEnd` |
| `message_delta` | `stop_reason` → `Finish`；累加 `usage.output_tokens` |
| `message_stop` / `ping` | 忽略 |
| `error` | 当作流内错误，交给上层按可重试处理 |

`stop_reason` 映射：`end_turn`→stop、`max_tokens`→length、`tool_use`→tool_calls、
`refusal`→content_filter、其它→error（原文留在 `Finish::raw`）。

**错误分类**：429 与 5xx 可重试，`overloaded_error`(529) 可重试并尊重 `retry-after`；
400 且 message 含 `prompt is too long` 归为 `context_too_long`。

**extended thinking 先不开**：打开后带工具的多轮要求把 thinking 块连同 signature 原样回传，
漏一个就 400。P5 先把不带思考的路径跑通，`reasoning_signature` 字段留着，等真有需求再单独做一版。

## 3. ollama 原生

不能只用它的 `/v1` 兼容端点：那里设不了 `options.num_ctx`，而 Ollama 是按 `num_ctx` 截断上下文的，
默认值远小于模型上限，超出部分**静默丢弃**，不报错也不在 usage 里体现。我们必须显式传。

| 项 | 值 |
| --- | --- |
| 端点 | `POST {base_url}/api/chat`，默认 `http://127.0.0.1:11434` |
| 鉴权 | 无 |
| 流 | **NDJSON**，每行一个完整 JSON（需要 `Framing::ndjson`，见 [02 §4](02-provider.md)） |
| 增量 | 每行 `message.content` 追加；`message.thinking` → `ReasoningDelta` |
| 工具 | 整包给出，没有 id：provider 自己编号并在本次请求内记映射 |
| 结束 | `done:true` 一行带 `done_reason`、`prompt_eval_count`、`eval_count` |
| 上下文 | 请求里显式传 `options.num_ctx = context_window`（配置里的值）；不传就会被默认值静默截断 |
| 思考 | 顶层 `think: true` 打开，增量在 `message.thinking` |

## 4. 怎么验收

| Provider | 怎么验 |
| --- | --- |
| openai-chat | 本机 llama.cpp（已有）+ 你手上的 DeepSeek / 智谱 key 各跑一轮 |
| anthropic | 需要一个能用的 key；跑「读文件 → 改文件 → 总结」一轮，外加一次故意超长、一次故意 429 |
| ollama 原生 | 本机 Ollama（已在跑，0.30.11）拉一个带 tools 能力的小模型；验证 NDJSON 分帧、工具调用编号，以及把历史撑过默认 `num_ctx` 后没有静默截断 |

**跑不通就不合并**：拿不到 key 的 provider 停在代码评审，不写进 `design/llm.md` 的能力表。

## 5. 完成标准

1. 每个合并进来的 provider 都有一次真实运行记录（模型名、做了什么、看到什么）。
2. 三家的 `classify` 都试过 429 与上下文超长两种情况。
3. `design/llm.md` 的能力表只列真跑通的，未验证的写在已知限制里。
