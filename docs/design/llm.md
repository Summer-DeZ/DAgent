# llm：模型客户端与 Provider 编解码

模块 `llm`，库 `dagent_llm`，头文件在 `src/public/llm/`，实现在 `src/private/llm/`，命名空间 `dagent::llm`。
只依赖 agent（中立消息与 `ModelSession` 端口）、net 与 base；认证实现另外直接使用 libcurl。核心只传中立 `Request`、消费 `Reply`；
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
| `agent/reply.hpp`、`agent/port/model.hpp`（核心） | StreamEvent、Reply、Usage、ModelError、RetryOptions 与 `ModelSession::complete` 端口 |
| `llm/codec.hpp` | Codec 接口与协议错误 `Error` |
| `llm/provider.hpp` | ProviderConfig、ProviderInfo、Framing、providers、find_provider、make_codec |
| `llm/provider_detail.hpp` | Provider 内部共用的报文原语与工厂声明，不供装配使用 |
| `llm/model.hpp` | `Model`（实现 `agent::ModelSession`）与 `make_session` 工厂 |
| `llm/llm.hpp` | `to_public`：ProviderConfig → 无凭据的 `agent::PublicModel` |
| `provider.cpp` / `provider_chat.cpp` / `provider_ollama.cpp` / `provider_anthropic.cpp` | kind 注册表、工厂、通用 HTTP 错误分类；各协议的 encode、decode、classify |

`ProviderConfig` 包括 kind、配置名字、base_url、model、api_key、login、内部凭据路径、headers、max_tokens、temperature、
context_window、send_reasoning_content、include_usage 和 extra_body。只存在于 app 装配与 llm 内部；
`PublicModel.auth` 只公开认证来源（`key` / `login:opencode` / 空字符串），`has_key` 为旧前端保留。
配置加载不读凭据，未登录的条目不会阻止其它模型使用。Codec 有流状态，不能跨请求复用。

`headers` 使用共用解析与校验，生成模型和 decision_models 都支持。加载时展开 `${ENV}`（未设置或空值报错），
每次 encode 替换 `{session_id}`，其它花括号保持原样；没有会话身份却使用占位符时报错。
头名必须是非空 HTTP token，展开与替换后的值都不得包含 CR/LF/NUL、其它 ASCII 控制字符或 DEL。
协议内置头 → 登录提供方默认头 → 条目 headers，按 ASCII 大小写无关合并，后者覆盖前者；
同一配置对象的大小写重复键报错，不支持删除与多值头。

全局保留 Host、Content-Length、Transfer-Encoding、Connection、TE、Trailer、Upgrade、Expect、
Proxy-Authorization、Proxy-Authenticate、Content-Type、Accept 和 User-Agent。
Anthropic 下另保留 anthropic-version；有托管认证（api_key 字段或 login）时保留 Authorization 与 x-api-key。
没有托管认证时可在 headers 中自行设置认证头。User-Agent 只在 HTTP 层设置为 `DAgent/<构建版本>`。
请求头值不投影到 UI、公开清单、日志、workspace.info 或会话记录。

`ProviderInfo` 声明默认端点、SSE/NDJSON 分帧、是否必须提供 key/max_tokens。
`make_codec` 对未知 kind 抛 `invalid_argument`。不探测模型能力或上下文窗口。

## 2. 中立格式

Message 保留 role、content、reasoning_content、tool_calls、tool_call_id，并补充
`reasoning_signature`（无校验串时为空）。内容块、厂商字段名和 URL 不进入消息模型。
`ReasoningDelta` 的可选 signature 增量由 Model 累积到 Message，界面只显示文本。
会话 assistant 记录必须包含 `reasoning_signature`；没有校验串时写空字符串，缺失字段直接报错。

`Request::session_id` 来自请求所属会话的 `SessionMeta::id`，不绑定到可共享的 `ModelSession`。
主轮次经 `Session::request_shape` / `Conversation::build` 传递，压缩摘要沿用同一 RequestShape 的 ID，
父级审阅使用执行审阅的父会话 ID；子 agent 的轮次和压缩使用自己的会话 ID。
重试保留请求 ID，恢复会话沿用存储中的 ID，新会话使用装配层已有的 ID 生成流程。
此字段仅用于请求头替换，不进入发给模型的消息或 JSON 正文。d3 的 `DecisionRequest.session_id` 从被审阅动作的
`ApprovalIdentity.child_session_id` 取得，包括网页内容扫描；decision_models 支持 headers，但禁止 login。

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
结束原因识别 stop、length、tool_calls、content_filter；缺失或未知值归 error。

配置 `max_tokens` 是唯一输出预算来源，Chat 请求映射为 `max_completion_tokens`。
`extra_body` 通过公共 `merge_extra` 只补缺失字段，不覆盖已生成字段；不得另行配置输出预算字段。

### ollama

POST `{base_url}/api/chat`，默认不添加鉴权头。
工具参数转换成对象；服务端没有调用 ID，适配器生成每次请求唯一的前缀与顺序号，
回传历史时由 ID 找工具名并按原顺序发送 tool 消息。这样不会覆盖界面中更早的工具卡片。
message.content / thinking 映射为文本/思考增量；done=true 时交付 usage 和 finish。
stop 且出现工具调用归 tool_calls，length 归 length，load/unload 等未知结束原因归 error。

总是显式设置 `options.num_ctx` 为有效窗口，`num_predict` 为输出上限。
`extra_body.options` 仅补充未生成的采样参数，不能覆盖 num_ctx；think、keep_alive 等顶层参数可透传。
窗口为 0 时由装配统一回落到全局 `context.window_tokens`，相同窗口同时用于压缩预算和请求。
接口依据 [Ollama Chat 文档](https://docs.ollama.com/api/chat)。

## 4. 已实现但未验收：Anthropic

`anthropic` kind 已在代码中提供，默认端点为 `https://api.anthropic.com/v1`；仓库尚无真实服务验收记录，
不能将“可配置、可构建”写成“已通过真实验收”。

实现 POST `/messages`、x-api-key 与 anthropic-version 头，强制非零 max_tokens。
system 单独放顶层；assistant 工具调用翻译为 tool_use，连续工具结果合并为同一 user 消息的 tool_result。
SSE 按块 index 处理文本及工具 JSON 分片；message_start 汇总输入和缓存用量，
message_delta 使用累计 output_tokens。end_turn/stop_sequence、max_tokens、tool_use、refusal
分别归 stop、length、tool_calls、content_filter。

extended thinking 尚未作为独立能力开放；`extra_body` 中的 `thinking` 会原样并入请求体，由调用方自行承担协议兼容性。
虽然保留思考文本和 signature 的流/记录通道，仍未声称支持完整 thinking 块回传。
实现依据 [Claude 流式接口文档](https://platform.claude.com/docs/en/build-with-claude/streaming)。

## 4.1 决策适配器（systemone）

决策审阅使用独立的 `DecisionModel` 端口，不与生成模型 `ModelSession` 共用会话历史、消息或 token 预算：

| 文件 | 职责 |
| --- | --- |
| `agent/port/decision.hpp` | `DecisionModel::evaluate(request, deadline, stop)` 与 `DecisionReviewStore` 端口 |
| `llm/decision_provider.*` | `DecisionProviderConfig`、适配器目录与 `make_decision_model` 工厂 |
| `llm/decision_systemone.*` | `POST /v1/systemone`：编码 state + questions，解码 `answers.<键>.noul` / `.choice` 与 `probabilities` |

- `criteria` 为空的问题编码成 `type: "noul"`，否则 `type: "choice"`；不使用 choice 的 `confidence` 字段。
- 超时从提交请求起计入排队与 HTTP 等待（默认 10 秒，可被整体 deadline 收窄）；失败与响应不合法统一返回
  `DecisionError`，不静默重试，不补分数。同一实例串行使用一个 `HttpClient`（服务只有单个处理槽）。
- 响应必须匹配配置的模型标识，并提供所有请求类别的有限概率（0–1）；不完整、类型错误或越界的响应转后续审批。
- 问句由核心 `agent/decision_review` 提供并带版本号；适配器不生成或改写问句。
- 维度键统一使用核心的名称映射；风险阶段的三个布尔分数独立返回，不要求总和为 1。
  MCP 第一阶段同时询问 direction，授权阶段按需再请求 authorization；低风险不请求授权分数。
- `endpoint` 是完整地址；`model_revision` 由部署方在权重或量化变化时更新，阈值按它隔离。

## 5. 错误与预算

共享分类支持 408、429、5xx 重试（含 529），解析 retry-after 秒数或 HTTP-date。
400 的上下文超长措辞归 context_too_long，包括 `prompt is too long` 和 llama.cpp 的 available context size。
每次尝试收集实际 token、静态 key、环境展开和会话替换后的头值，统一脱敏。
HTTP 错误正文（包括 JSON 转义值）在 classify 与诊断截断前脱敏，Finish.raw 在事件外发、记录和日志前脱敏、修复 UTF-8 并截断。
HTTP 层已因上限截断的错误体整体替换为固定说明，避免暴露被截断的敏感值前缀。退避与重试上限由 Model 管理，强制压缩由核心 TurnRunner 管理。
HTTP 错误正文摘要和非法 SSE data 的诊断先修复 UTF-8，再按字符边界截断，避免重试及结束事件无法序列化。

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
| 本轮共享调用次数或 token 预算不足 | `budget_exhausted`，下一次尝试不发送，核心以 `limit` 收尾 |

默认最多重试 2 次，即最多 3 次尝试。退避从 1 秒开始指数增长，以 30 秒为基础上限，增加 0.8–1.2 倍随机抖动；
服务端的 Retry-After 更长时采用它，超过 300 秒则直接失败。等待可取消；已输出过内容的失败尝试发 StreamReset 让前端清除，
不写入历史，重试期间取消不会保存已作废尝试的半截内容。

普通模型使用配置的总超时、空闲超时、连接超时与 TLS 配置；login 模型强制 verify_peer/verify_host。
HTTP 不跟随重定向。
随附总超时为 0（不限）、空闲超时为 300 秒：流式回复只要持续收到字节就不判超时，长时间的思考与输出不会被总时长截断；0 的语义见 [HTTP 选项](net.md#选项httpoptions)。
idle timeout 看收到的字节，包括 SSE 注释；预填充期间服务端通常不发字节，300 秒覆盖单卡上约 15 万 token 的冷预填充，更长的上下文或更慢的服务需按实际模型调整配置。

`ModelAttemptHooks` 在每次实际尝试前后调用，包括重试。核心的 `BudgetedModel` 在发送前检查共享预算，
收窄 `max_tokens` 并计数，结束后累计已报告 usage 或估算未报告消耗；重试等待本身不计下一次尝试。
父模型审阅另受包含排队时间的 `subagents.approval.review_timeout_ms` 约束。

## 7. 登录提供方与凭据（阶段 3a）

`llm/auth.hpp` / `auth.cpp` 内置唯一提供方 opencode：设备码与令牌端点为
`https://opencode.ai/console/auth/device/code`、`https://opencode.ai/console/auth/device/token`，
client_id=dagent、supports_org_scope=true，默认头 `x-opencode-session: {session_id}`。
当前只开放 openai-chat，base_url 必须匹配 `/inference/go/openai/v1`；Anthropic Go 地址仅用于清单中显示不可用项。
用 libcurl URL API 检查最终编码 URL 的 HTTPS origin 和精确路径，禁止 userinfo；不按字符串前缀匹配主机。
认证和带登录 token 的推理都强制 TLS 校验且不跟随重定向。

RFC 8628 轮询缺省间隔 5 秒，slow_down 每次增加 5 秒，连接超时后指数降低轮询频率；
authorization_pending 继续，拒绝、过期和其它 OAuth 错误终止。批准链接相对提供方 origin 解析并校验。
设备等待按授权期限截止；认证 HTTP 总超时 30 秒、连接超时 10 秒、响应上限 64 KiB；锁等待上限 30 秒。
以上等待与请求均可取消。OAuth 失败仅显示 HTTP 状态、白名单错误码与固定说明，不透传响应或 JSON 解析异常。

HomePaths 提供 `data/credentials/opencode.json` 和 `run/credentials/opencode.lock`。
POSIX 凭据目录 0700、文件 0600；逐级拒绝 symlink，文件须属当前用户且为普通文件。
临时文件从创建起 0600，写入后 fsync → rename → fsync 目录。
字段 access_token、refresh_token、expires_at、org_id、client_id、state、updated；时间为 Unix 秒。
登录保存、刷新与登出共享固定 flock 文件；登出保留锁文件。设备等待不持锁，推理不持锁。
每次请求重读凭据，不设进程内 token 缓存。

`auth.cpp` 保留 OAuth、刷新流程、URL 校验、JSON 编解码、状态投影和可取消等待。
`auth_{posix,win}.cpp` 实现目录遍历、文件验证、CredentialLock 及凭据文件读写/删除，
由 CMake 的 `auth_${DAGENT_PLATFORM}.cpp` 选择。内部接口在 `llm/auth_detail.hpp`，不暴露原生句柄。

POSIX 目录遍历从 `/` 开始，逐级 openat 使用 `O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC`；
允许创建时，仅在 ENOENT 分支 mkdirat(0700) 后重开。每级取得新句柄后关闭前一级，最终目录才核对 uid 和 0700。
文件验证保持 fstat、普通文件、当前有效 uid 和权限低 9 位为 0600 的检查。

- 读取：不创建目录；目录或文件不存在返回空。openat 使用 `O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC`，
  验证后以 4096 字节块读取，EINTR 重试，上限 64 KiB。原 try 块中调用共享 `decode_credentials`，
  JSON 异常仍返回 state=invalid；解析期间文件和目录句柄都保持打开，退出时先关闭文件再关闭目录。
- 写入：打开/创建目录，打开并验证已有文件，再生成原临时名称，以
  `O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC`、0600 打开临时文件。
  随后的原 try 块才调用共享 `credential_json` 并 dump、追加换行；EINTR 重试，零或其他负写入返回报错。
  保留短路求值的 fsync(文件) → renameat → fsync(目录)；异常先 unlinkat 临时名称再重抛。
  临时文件和已有文件句柄一直持有到写入函数退出，依次关闭临时文件、已有文件、目录。
- 删除：logout 仍先加锁并 read_credentials；存在时调用 `remove_credentials`，不创建目录，
  unlinkat 成功才 fsync。删除目录句柄在释放 CredentialLock 前关闭，不删除锁文件。
- 加锁：CredentialLock 的 Impl 在目录操作前分配，先持有锁目录，再以
  `O_CREAT | O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC`、0600 打开并验证锁文件。
  验证后设 30 秒期限，每轮先检查取消，再尝试非阻塞 flock；仅 EWOULDBLOCK/EINTR 进入期限检查和 50ms 可取消等待。
  失败和正常析构都先关闭锁文件、再关闭锁目录，通过 close 释放锁，不增加显式 UNLOCK。

Windows 的对应实现以 NtCreateFile 的 RootDirectory 逐分量打开目录和文件，不跟随 reparse point，
并拒绝 ADS。仅在最终凭据目录核对当前用户所有及受保护私有 DACL；既有祖先不被重新授权。
新目录、锁文件和随机独占临时文件在创建时就传入私有描述符，当前用户/SYSTEM/Administrators 有完全控制，
禁止继承父目录 ACL。已有凭据或最终目录不安全时拒绝操作，不先读取明文再补权限，也不自动修复。
校验拒绝 NULL DACL、继承未关闭、其他获准主体和非普通 allow ACE；要求当前用户读写，目录另需创建/遍历。
只处理 owner/DACL，不使用管理员提升或修改特权 SACL。

Windows 锁使用同步 LockFileEx(EXCLUSIVE|FAIL_IMMEDIATELY) 锁定 [0,1)，仅 ERROR_LOCK_VIOLATION 进入
原 30 秒期限检查和 50ms 可取消等待；取消继续使用共享 check_stop/pause。析构关闭文件再关闭目录，锁随文件句柄释放。
读取仍分 4096 字节块、限制 64 KiB，使用原 decode_credentials；JSON 异常仍得到 state=invalid。
写入使用原 credential_json().dump()+换行，完整写入后 FlushFileBuffers，再相对固定目录用 FileRenameInfoEx 原子替换；
保留已有验证句柄，失败时删除临时文件。登出通过 FileDispositionInfoEx 删除凭据，保留锁文件。
目录 flush 在 Windows 为尽力而为；不支持扩展替换/删除的文件系统直接报错，不降级为先删后写。

文件/目录/普通 I/O 错误仍为 `credential storage is unavailable or unsafe`；
锁超时为 `credential lock wait timed out`，取消仍抛 HttpError(cancelled, `authentication cancelled`)。

到期前 5 分钟或 HTTP 401 触发刷新：锁内重读，若 token 已变化则采用其它进程刷新的结果；
否则先原子保存 refresh_pending，再用 refresh token 请求新令牌并保存 ready。
CredentialLock 仍在共享流程的原作用域内，覆盖重读 → refresh_pending 写入 → HTTP → 新凭据写入。
残留 refresh_pending、invalid_grant 或不确定的刷新结果均要求重新登录，不再重发旧 refresh token。
刷新收到有效结果后即使取消也先保存轮换结果。

每次 complete 最多一次 401 恢复，在 classify 前按状态码识别，403 不恢复；额度不随网络重试重置。
恢复重发保持正文与 session_id，每次真实发送均执行 ModelAttemptHooks，401 按零推理 usage 结算但计一次尝试。
若下一次预算钩子要求缩小正文中的输出上限，则以 budget_exhausted 结束，避免改变重发正文或超预算。
408/429/5xx、流中断沿用既有重试与退避。

本阶段仅完成构建验证；浏览器登录、真实 Go 推理、并发刷新、目录权限攻击与错误脱敏仍须真实运行验收。
