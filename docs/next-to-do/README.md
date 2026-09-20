# Provider：用户自己切换模型

目标：用户在配置里写几套模型，运行时用 `/model` 切换；核心只认一套**内部统一格式**，
外部各家的报文差异全部收在 Provider 里。

先把话说明白：**内部格式已经有了**。[`agent::Message` / `Request`](../../src/public/agent/message.hpp)
是中立消息模型，[`StreamEvent`](../../src/public/agent/llm.hpp) 是中立流事件，
[`Codec`](../../src/public/agent/llm.hpp) 已经是「内部 ↔ 外部」的翻译层。这次要做的是：

1. 把「只有 openai-chat 一家」的假设从核心里拔掉（`Setup` 现在直接持有 `OpenAiChatOptions`）；
2. 补内部格式的两处缺口（流分帧、思考回传）；
3. 让模型配置从一段 `gateway` 变成一张可命名的 `models` 表（**旧段直接删掉，不留兼容**），并能在界面里切换。

---

## 1. 范围

### 做

| Provider kind | 覆盖的服务 | 说明 |
| --- | --- | --- |
| `openai-chat` | OpenAI、DeepSeek、智谱、Qwen、OpenRouter、llama.cpp、vLLM、LM Studio、Ollama 的 `/v1` | 已有实现，改造成 provider 之一 |
| `anthropic` | Claude Messages API | 新写；报文差异最大的一家 |
| `ollama` | Ollama 原生 `/api/chat` | 必做，原因见下 |

**Ollama 必须走原生 `/api/chat`**，不能只靠它的 OpenAI 兼容端点。兼容端点设不了 `options.num_ctx`，
而 Ollama 的实际上下文是按 `num_ctx` 截断的，默认值远小于模型上限——不显式传，长对话会被**静默截掉**，
没有任何报错。`think`、`keep_alive` 也只在原生接口上。代价是它的流是 NDJSON 而不是 SSE
（[02 §4](02-provider.md)）。

### 不做

| 不做 | 原因 |
| --- | --- |
| Gemini 原生 `generateContent` | 官方有 OpenAI 兼容端点，走 `openai-chat`（代价是拿不到 `inputTokenLimit`，窗口要写在配置里） |
| OpenAI Responses API | Chat Completions 够用，Responses 的状态语义会污染内部格式 |
| 同一轮里多模型路由、自动降级 | 一轮一个模型，先把切换做对 |
| 任何形式的自动探测（能力、上下文窗口） | 一律由配置声明；云端几家根本探不到，Ollama 探到的是模型上限而非生效值（[01 §2.3](01-internal.md)） |
| 价格与用量计费 | 没有可靠的价目数据源 |

### 一条硬规则

**没有真实跑通的 provider 不合并**。`anthropic` 需要一个能用的 key 才能验收；
拿不到就先停在代码评审，不写进设计文档当作已支持的能力（沿用 [llm 设计文档](../design/llm.md) 的既有原则）。

## 2. 里程碑

| | 里程碑 | 做完能看到 | 任务 |
| --- | --- | --- | --- |
| P1 | 内部格式补齐 | 内部结构能表达三家的请求与流，且不含任何厂商字段名 | [01-internal](01-internal.md) |
| P2 | Provider 抽象 | `Setup` 不再认识 `OpenAiChatOptions`；`make_codec(kind)` 造编解码器；行为与现在一致 | [02-provider](02-provider.md) |
| P3 | 多模型配置 | 配置里的 `models` 表 + `model` 默认项 + `--model`；`gateway` 段删除 | [03-config](03-config.md) |
| P4 | 运行时切换 | `/model` 面板切换，会话 id 不变、历史保留、侧栏与尾行跟着变 | [04-switch](04-switch.md) |
| P5 | Anthropic 与 Ollama 适配 | Claude 跑完一轮带工具的对话；Ollama 原生跑通并确认 `num_ctx` 生效 | [05-adapters](05-adapters.md) |
| P6 | 文档 | `design/llm.md` 重写 provider 一节，`design/app.md` 补配置，`design/ui.md` 补 `/model` | — |

```
P1 ─► P2 ─► P3 ─► P4
        └─► P5（抽象落地后才谈得上第二家）
```

P3 与 P5 之间没有依赖，谁先做都行；P4 需要 P3 的 models 表。

## 3. 验收

每个里程碑都要在真实服务上跑，不做模拟：

| # | 场景 | 看什么 | 里程碑 |
| --- | --- | --- | --- |
| 1 | 本地 llama.cpp 跑完一轮带工具的对话 | 与改造前完全一致（P2 是纯重构） | P2 |
| 2 | 配置三套模型，`--model glm` 启动 | 启动用的是指定的那套，密钥取对 | P3 |
| 3 | 写错 `kind`、漏 `model`、密钥变量不存在 | 启动时报错指明是哪个模型的哪个键 | P3 |
| 4 | `/model` 在本地模型与远端模型之间来回切 | 会话 id 不变、历史还在、上下文上限按新模型更新、侧栏与输入框尾行同步 | P4 |
| 5 | 切换后接着让它调工具 | 工具调用与回填正常，没有残留上一家的 id 格式 | P4 |
| 6 | Claude 跑一轮：读文件 → 改文件 → 总结 | 工具调用、usage、finish_reason、错误分类都对 | P5 |
| 7 | 故意超长上下文、故意 429 | 分类成 context_too_long / retryable，重试与压缩照常 | P5 |
| 8 | Ollama 原生跑一轮，故意让历史超过默认 `num_ctx` | 没有静默截断：要么按配置的窗口正常工作，要么明确报上下文超长 | P5 |
| 9 | 会话记录回放（切过模型的那个会话） | 回放不因换模型而断裂 | P4 |

## 4. 进度

| 里程碑 | 状态 |
| --- | --- |
| P1 内部格式 | 已实现；旧会话真实恢复通过 |
| P2 Provider 抽象 | 已实现；本地 Qwen CLI / PTY 工具回合通过 |
| P3 多模型配置 | 已实现；选择、分层合并、校验与列表通过 |
| P4 运行时切换 | 已实现；本地跨协议切换和回放通过，远端待密钥 |
| P5 Ollama 原生 | 已实现；工具读写、num_ctx 与 12,788 token 长输入通过 |
| P5 Anthropic | 代码及构建完成；无密钥，未真实验收，不合并 |
| P6 文档 | 已同步 design/llm、app、ui、agent |

完整证据、临时产物路径与未覆盖项见 [验收记录](validation.md)。
