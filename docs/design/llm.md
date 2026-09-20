# LLM 编解码：消息模型与 Provider

属于 `dagent_agent`，头文件在 `src/public/agent/`，实现在 `src/private/agent/`。
核心只传中立 `Request`、消费 `StreamEvent`；Provider 翻译 URL、鉴权、报文和错误。
重试、取消、工具执行和压缩仍由 [agent 运行时](agent.md) 负责。

```text
ProviderConfig → make_codec（每次请求一个实例）
Request → encode → HTTP → SSE / NDJSON → decode → StreamEvent
非 2xx → classify → Error → Model 决定重试或交给 Agent 压缩
```

## 1. 接口与边界

| 文件 | 职责 |
| --- | --- |
| `agent/message.hpp` | Role、Message、ToolCall、ToolDef、Request |
| `agent/llm.hpp` | StreamEvent、Error、Codec、token 估算 |
| `agent/provider.hpp` | ProviderConfig、ProviderInfo、Framing、providers、find_provider、make_codec |
| `agent/provider_detail.hpp` | Provider 内部共用的报文原语与工厂声明，不供 app 装配使用 |
| `provider.cpp` | kind 注册表、工厂、通用 HTTP 错误分类 |
| `provider_chat.cpp` / `provider_ollama.cpp` / `provider_anthropic.cpp` | 各协议的 encode、decode、classify |

`Setup::provider` 替代原来的模型参数与专用 Codec 选项。`ProviderConfig` 包括 kind、配置名字、
base_url、model、api_key、max_tokens、temperature、context_window、send_reasoning_content、
include_usage 和 extra_body。app 解析密钥，核心不读取环境或配置文件。Codec 有流状态，不能跨请求复用。

`ProviderInfo` 声明默认端点、SSE/NDJSON 分帧、是否必须提供 key/max_tokens。
`make_codec` 对未知 kind 抛 `invalid_argument`。不探测模型能力或上下文窗口。

## 2. 中立格式

Message 保留 role、content、reasoning_content、tool_calls、tool_call_id，并补充
`reasoning_signature`（无校验串时为空）。内容块、厂商字段名和 URL 不进入消息模型。
`ReasoningDelta` 的可选 signature 增量由 Model 累积到 Message，界面只显示文本。
会话 assistant 记录保存 signature；旧记录缺该字段时按空读取。

StreamEvent 仍是 TextDelta、ReasoningDelta、ToolCallBegin/Delta/End、Usage 和 Finish。
Finish 只分 stop、length、tool_calls、content_filter、error；未知原因保存原文到 raw，并归 error。
流缺少结束事件或返回流内错误时，Model 重试；不会把不完整回复当成功。
Usage 使用累计值，避免把累计计数再次相加。

`Model` 按 ProviderInfo 分帧：SSE 使用 `net::SseParser`；NDJSON 缓冲跨网络块的半行，
按换行分出完整 JSON，处理 CRLF 和最后没有换行的完整行，再包装成 `SseEvent.data` 交给 Codec。
网络层无需知道厂商协议。

## 3. 已真实运行的 Provider

| kind | 默认 base_url | 分帧 | 本次证据 |
| --- | --- | --- | --- |
| `openai-chat` | `https://api.openai.com/v1` | SSE | 本地 Qwen3.8-Flash-Next，工具调用、恢复、跨协议切换 |
| `ollama` | `http://127.0.0.1:11434` | NDJSON | qwen3:1.7b，读写工具、切换、12,788 prompt tokens、num_ctx=16384 |

运行记录与未覆盖项见 [Provider 验收记录](../next-to-do/validation.md)。表中的协议已验证，
不代表同协议下每一个远端服务都已经验收。

### openai-chat

POST `{base_url}/chat/completions`，非空 key 使用 Bearer 头。
工具参数以 JSON 字符串发送；纯工具调用的 assistant.content 为 null。
默认请求 `stream_options.include_usage=true`；可按配置关闭。
历史思考默认不回传，显式 `send_reasoning_content=true` 时才发送。
流同时识别 reasoning_content / reasoning，前者出现时优先；工具分片按 index 聚合，
在 `[DONE]` 时依次交付 ToolCallEnd、Usage、Finish。

`extra_body` 顶层只补缺失字段，不覆盖已生成字段。配置 `extra_body.max_completion_tokens`
时不再生成 max_tokens；app 同时用该值作为输出预留预算，避免实际输出上限与预算脱节。

### ollama

POST `{base_url}/api/chat`，不添加鉴权头。
工具参数转换成对象；服务端没有调用 ID，适配器生成每次请求唯一的前缀与顺序号，
回传历史时由 ID 找工具名并按原顺序发送 tool 消息。这样不会覆盖界面中更早的工具卡片。
message.content / thinking 映射为文本/思考增量；done=true 时交付 usage 和 finish。
stop 且出现工具调用归 tool_calls，length 归 length，load/unload 等未知结束原因归 error。

总是显式设置 `options.num_ctx` 为有效窗口，`num_predict` 为输出上限。
`extra_body.options` 仅补充未生成的采样参数，不能覆盖 num_ctx；think、keep_alive 等顶层参数可透传。
窗口为 0 时由 Agent 统一回落到全局 `context.window_tokens`，相同窗口同时用于压缩预算和请求。
接口依据 [Ollama Chat 文档](https://docs.ollama.com/api/chat)。

## 4. 已实现但未验收：Anthropic

`anthropic` kind 可配置并构建，默认端点为 `https://api.anthropic.com/v1`，但当前没有可用密钥，
**不作为已验证能力发布或合并**。

实现 POST `/messages`、x-api-key 与 anthropic-version 头，强制非零 max_tokens。
system 单独放顶层；assistant 工具调用翻译为 tool_use，连续工具结果合并为同一 user 消息的 tool_result。
SSE 按块 index 处理文本及工具 JSON 分片；message_start 汇总输入和缓存用量，
message_delta 使用累计 output_tokens。end_turn/stop_sequence、max_tokens、tool_use、refusal
分别归 stop、length、tool_calls、content_filter。

extended thinking 尚未开放；配置试图启用时明确拒绝。虽然保留思考文本和 signature 的流/记录通道，
仍未声称支持完整 thinking 块回传。实现依据 [Claude 流式接口文档](https://platform.claude.com/docs/en/build-with-claude/streaming)。

## 5. 错误与预算

共享分类支持 408、429、5xx 重试（含 529），解析 retry-after 秒数或 HTTP-date。
400 的上下文超长措辞归 context_too_long，包括 `prompt is too long` 和 llama.cpp 的 available context size。
错误文本移除当前配置的 API key。退避、重试上限和强制压缩仍由 Model / Agent 统一管理。

预算使用模型的 context_window；为 0 时使用全局窗口。减去 safety_margin_tokens 和输出预留得到 limit，
ContextUpdate 报告该可用预算。切换后重新建立估算器，首轮正常执行自动压缩。
TokenEstimator 仍用 ASCII 约 4 字节/token、非 ASCII 码点约 1 token 的启发式，结合实际 prompt usage 校正。
历史 reasoning 不进入静态估算，由真实用量校正吸收偏差。

尚未真实验证：Anthropic 全路径、远端兼容服务、429/retry-after、Anthropic/Ollama 超长错误分类。
不提供 Responses、Gemini 原生接口、同轮路由或自动降级。验证只使用真实服务，产物放在 `temp/`。
