# llm：模型客户端与 Provider 编解码

模块 `llm`，库 `dagent_llm`，头文件在 `src/public/llm/`，实现在 `src/private/llm/`，命名空间 `dagent::llm`。
只依赖 agent（中立消息与 `ModelSession` 端口）、net 与 base。核心只传中立 `Request`、消费 `Reply`；
llm 负责 URL、鉴权、报文、流式累积、重试和错误分类。工具执行与压缩仍由 [agent](agent.md) 负责。

```text
ProviderConfig → make_codec（每次尝试一个实例）
Request → encode → HTTP → SSE / NDJSON → decode → StreamEvent → Model 累积 → Reply
非 2xx → classify → Error → Model 决定重试；上下文超长交给核心压缩
```

## 1. 接口与边界

| 文件 | 职责 |
| --- | --- |
| `agent/message.hpp`（核心） | Role、Message、ToolCall、ToolSpec、ModelParams、Request |
| `agent/reply.hpp`、`agent/port_model.hpp`（核心） | StreamEvent、Reply、Usage、ModelError、RetryOptions 与 `ModelSession::complete` 端口 |
| `llm/codec.hpp` | Codec 接口与协议错误 `Error` |
| `llm/provider.hpp` | ProviderConfig、ProviderInfo、Framing、providers、find_provider、make_codec |
| `llm/provider_detail.hpp` | Provider 内部共用的报文原语与工厂声明，不供装配使用 |
| `llm/model.hpp` | `Model`（实现 `agent::ModelSession`）与 `make_session` 工厂 |
| `llm/llm.hpp` | `to_public`：ProviderConfig → 无凭据的 `agent::PublicModel` |
| `provider.cpp` / `provider_chat.cpp` / `provider_ollama.cpp` / `provider_anthropic.cpp` | kind 注册表、工厂、通用 HTTP 错误分类；各协议的 encode、decode、classify |

`ProviderConfig` 包括 kind、配置名字、base_url、model、api_key、max_tokens、temperature、context_window、
send_reasoning_content、include_usage 和 extra_body。它只存在于 app 装配与 llm 内部：app 解析 `env:` 密钥后调用
`make_session` 得到已配置的 `ModelSession`，核心、runtime 和前端只见 `PublicModel`（has_key 表示是否有密钥）。
Codec 有流状态，不能跨请求复用。

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

表中的协议已用列出的服务真实验证，不代表同协议下每一个远端服务都已经验收。

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
窗口为 0 时由装配统一回落到全局 `context.window_tokens`，相同窗口同时用于压缩预算和请求。
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
错误文本移除当前配置的 API key。退避与重试上限由 Model 管理，强制压缩由核心 TurnRunner 管理。

预算使用模型的 context_window；为 0 时使用全局窗口。减去 safety_margin_tokens 和输出预留得到 limit，
ContextUpdate 报告该可用预算。切换后重新建立估算器，首轮正常执行自动压缩。
TokenEstimator 仍用 ASCII 约 4 字节/token、非 ASCII 码点约 1 token 的启发式，结合实际 prompt usage 校正。
历史 reasoning 不进入静态估算，由真实用量校正吸收偏差。

尚未真实验证：Anthropic 全路径、远端兼容服务、429/retry-after、Anthropic/Ollama 超长错误分类。
不提供 Responses、Gemini 原生接口、同轮路由或自动降级。验证只使用真实服务，产物放在 `temp/`。

## 6. Model：一次完整调用与重试

`Model::complete` 把中立 `Request` 经 Codec 和流式 HTTP 转成 `Reply{message, finish, usage}`，同时把可见增量
（正文、思考、ToolPending、Retrying、StreamReset）交给核心传入的接收器。每次尝试使用新的 Codec 和分帧解析器；
HttpClient 在同一执行线程上复用。工具调用按 index 归集参数，按首次出现顺序进入回复，补齐空 id、处理重复 id；
参数 JSON 留给工具的 prepare 解析，报错可回填给模型修正。

| 失败 | 处理 |
| --- | --- |
| stop 已请求 | `ModelError::cancelled`，带当前尝试的部分回复 |
| 连接失败、传输中断、超时；HTTP 408 / 429 / 5xx；流错误或缺正常结束标记 | 整个请求重试 |
| HTTP 分类为上下文超长 | `context_too_long`，交给核心压缩 |
| TLS 问题、不可重试 HTTP 错误、2xx 却没有流事件 | `rejected` |
| 重试次数耗尽或 Retry-After 太长 | `exhausted` |

默认最多重试 2 次，即最多 3 次尝试。退避从 1 秒开始指数增长，以 30 秒为基础上限，增加 0.8–1.2 倍随机抖动；
服务端的 Retry-After 更长时采用它，超过 300 秒则直接失败。等待可取消；已输出过内容的失败尝试发 StreamReset 让前端清除，
不写入历史，重试期间取消不会保存已作废尝试的半截内容。

装配把模型 HTTP 的总超时设为 0；配置的 idle timeout 为 0 时补成 120 秒。连接超时和 TLS 选项沿用配置。
idle timeout 看收到的字节，包括 SSE 注释；大上下文预填充期间没有字节时仍可能超时，需按实际模型调整配置。
