# Agent 核心：总览

外围七个模块、tools 层和 LLM 编解码都已完成（见 [docs/README.md](../README.md)）。本目录是剩下的**核心**的
设计与执行计划：由哪些部件组成、每个部件的规则、部件之间怎样接起来、按什么顺序实现、怎样验收。

分工：**核心由你实现**，每完成一个里程碑交给我审核（流程见 §9）。整个核心完成后，把本目录改写成
`docs/design/agent.md`（agent 运行时）和 `docs/design/ui.md`（应用层界面），然后删除本目录。

文档是起点，不是合同：实现中发现哪条规则不合理，直接改，审核时说明理由。

---

## 1. 范围

### 核心要做的

| 部件 | 一句话 | 文档 |
| --- | --- | --- |
| 对外接口 | Event 事件流、Approver 权限询问、TurnStatus | [01-events](01-events.md) |
| Model | 一次流式模型调用：累积、失败分类、重试 | [02-model](02-model.md) |
| Conversation | 消息历史与协议不变式 | [03-conversation](03-conversation.md) |
| Agent 与一轮循环 | 模型 → 工具 → 回填，直到结束；中断、上限、结束原因 | [04-turn](04-turn.md) |
| 工具调度 | 一批工具调用的 prepare、权限、并行与串行 | [05-dispatch](05-dispatch.md) |
| 权限 | 按 Intent 决策、会话授权、四种模式 | [06-permission](06-permission.md) |
| 上下文管理 | token 预算、裁剪旧输出、摘要压缩 | [07-context](07-context.md) |
| 提示词 | system prompt 与摘要提示词：内置、覆盖、渲染 | [08-prompt](08-prompt.md) |
| 会话记录与恢复 | 核心的事件格式、落盘、回放重建、崩溃闭合 | [09-record](09-record.md) |
| McpHub | MCP server 的后台连接、刷新、重连 | [10-mcp](10-mcp.md) |
| 入口 | Options 与装配、`main` 四种模式、run 模式 | [11-entry](11-entry.md) |
| 交互界面 | 对话视图、权限对话框、输入与快捷键 | [12-ui](12-ui.md) |
| 执行计划 | 里程碑 C1–C6、任务拆分、验收场景 | [plan](plan.md) |

### 这一阶段不做

| 不做 | 原因 |
| --- | --- |
| 子 agent、todo 工具、web_fetch、图片输入 | 等最小循环跑通、看到真实使用情况再加（tools 文档的已知限制） |
| Anthropic / Responses 编解码器 | 本机没有可实测的服务，不写没验证过的代码（llm 文档） |
| 运行中切换模型、多模型路由 | 一个会话一个模型，先把单模型做对 |
| hooks、插件、自定义斜杠命令 | 没有真实需求 |
| 会话搜索索引 | session 文档：会话多到需要时再加 SQLite |
| bash 保留 cwd / 后台 shell | 和并行执行冲突，tools 文档已说明 |

---

## 2. 分层与依赖

```
dagent（可执行，只有 src/private/agent/main.cpp）
  ├─► dagent_app   ──► dagent_agent + 全部外围 + tools   配置、命令行；把配置映射成 agent::Options
  ├─► dagent_ui    ──► dagent_agent + dagent_tui + tools 只认 agent::Event、agent::Approval 和 tools::View
  ├─► dagent_tui                                         sessions 子命令按字素宽度排版标题（tui/grapheme.hpp）
  └─► dagent_agent ──► tools, session, mcp, workspace, exec, net, base
```

- **核心的代码全部放在 `src/*/agent`**，包括入口：`main.cpp` 和 run 模式都在 `src/private/agent/`。可执行目标
  `dagent` 只编 `main.cpp`；`dagent_agent` 这个库本身**不依赖 app、ui、tui**，所以 main 不进库。
- agent 对外只有两个接口：事件回调 `Sink` 和权限询问 `Approver`（[01-events](01-events.md)）。run 模式和交互界面
  是它们的两种实现；以后加 IDE 插件或 RPC 前端，agent 不用改。
- 交互界面按 TUI 冻结时的约定留在 `src/*/ui`，只用框架原语搭建，不改 `src/*/tui`。
- `context`、`run`、`progress`、`permissions` 四个配置段已在 C1 搬进 `agent::Options`，app 依赖 `dagent_agent`
  做映射（agent 不反向依赖）。

---

## 3. 文件布局

```
src/public/agent/                          src/private/agent/
  message.hpp      已有：中立消息模型          llm.cpp、openai_chat.cpp   已有
  llm.hpp          已有：StreamEvent、Codec
  openai_chat.hpp  已有
  events.hpp       Event、Sink、Approver     events.cpp       to_json
  options.hpp      Options、Setup            —
  model.hpp        Model、ModelError         model.cpp
  conversation.hpp Conversation              conversation.cpp
  permission.hpp   Policy                    permission.cpp
  compaction.hpp   Budget、Compactor         compaction.cpp
  prompt.hpp       system / compact 提示词    prompt.cpp       + 构建目录生成的 prompts.cpp
  record.hpp       Recorder、replay_into     record.cpp
  mcp_hub.hpp      McpHub                    mcp_hub.cpp
  agent.hpp        Agent                     agent.cpp        一轮循环
                                             dispatch.cpp     工具调度（Agent 的私有部分）
  headless.hpp     run 模式                   headless.cpp
                                             main.cpp         可执行 dagent，不进库

src/public/ui/                             src/private/ui/
  theme_config.hpp 已有                       theme_config.cpp 已有
  shell.hpp        run_interactive           shell.cpp
  transcript.hpp   Event → Document 块        transcript.cpp
  approval.hpp     权限对话框                  approval.cpp
  status_line.hpp  状态栏控件                  status_line.cpp
  prompt_input.hpp 输入处理（发送/排队/斜杠命令） prompt_input.cpp

prompts/   system.md  compact.md
cmake/     prompts.cpp.in          把 prompts/ 编进二进制的模板
temp/                             临时验收材料（小项目、故障注入代理、脚本）：随时可能清空，按需重建
```

---

## 4. 一轮对话的数据流

```
用户 ──输入──► Shell/headless ──run_turn(input, sink, approver, stop)──► Agent
                                                                        │
  ┌──────────── 每一步（step）──────────────────────────────────────────┤
  │ McpHub.apply_pending ─► Registry                                    │
  │ Compactor.maybe_compact ─► Conversation（可能调一次 Model 做摘要）     │
  │ Conversation.build ─► Request ─► Model.complete ─► StreamEvent ─► Sink │
  │ Conversation.add_assistant ─► Recorder                              │
  │ 有 tool_calls：dispatch                                              │
  │   ├─ Tool.prepare ─► Intent                                          │
  │   ├─ Policy.evaluate ─► 允许 / 询问（Approver）/ 拒绝                 │
  │   ├─ Call.run（并行组在工作线程上）─► ToolOutput ─► Sink              │
  │   └─ Conversation.add_tool_result ─► Recorder ─► ToolFinished ─► Sink │
  └─────────────────────────────────────────────────────────────────────┘
                                                                        │
                                              TurnEnded ◄── Recorder.turn_end + sync
```

---

## 5. 线程模型

| 线程 | 谁创建 | 做什么 |
| --- | --- | --- |
| 渲染线程 | 交互模式下的主线程，`Runtime::run()` | 控件树、Document、事件处理器 |
| agent 线程 | 交互模式：`ui::Shell` 建的 `std::jthread`；run 模式：主线程 | `Agent` 的全部方法：模型请求、串行工具、会话写入、MCP 合并 |
| 工具工作线程 | 调度器为一个并行组临时建的 `std::jthread`，同时至多 8 个 | 只读工具的 `Call::run` |
| MCP 连接线程 | McpHub 为每个 server 建一个 | `mcp::Client::connect` |
| stdio MCP 读取线程 | mcp 模块内部 | `on_tools_changed` 回调 |
| 信号线程 | run 模式的 main | `sigwait` SIGINT/SIGTERM → `request_stop()` |

规则：

1. **`Agent` 不是线程安全的**，所有方法都在 agent 线程上调。`session::Writer` 单写入者、`net::HttpClient` 不能跨线程
   并发、`tools::Registry` 没有锁，这三条都由这一条规则满足。
2. **`Sink` 必须线程安全**：`ToolOutput` 可能在工具工作线程上发出。交互界面的实现只做 `Runtime::post`；run 模式的
   实现加一把锁写 stdout。
3. **`Approver` 只在 agent 线程上调**，同一时刻至多一个；stop 请求后必须尽快返回（100 ms 内）。
4. **取消只有一个入口**：调用方持有 `std::stop_source`，任意线程 `request_stop()`。模型请求、重试等待、权限等待、
   工具执行、摘要请求都用同一个 token，要求即时生效，不能靠轮询间隔。
5. **Registry 只在两次模型请求之间改**（MCP 连接完成、`list_changed`、重连），不在工具执行中途改。

---

## 6. 统一约定

沿用外围的约定（阻塞调用 + stop_token + 回调、Options 是纯数据、不写冗余防御代码），另外：

### 6.1 错误

| 层 | 形式 |
| --- | --- |
| `Model::complete` | 抛 `ModelError{Kind}`：cancelled、context_too_long、rejected、exhausted（[02-model](02-model.md)） |
| `Agent::create` / `resume` | 抛外围异常原样（`SessionError`、`workspace::bad_template`……）：启动阶段的失败让入口报错退出 |
| `Agent::run_turn` / `compact` | **不抛异常**。一切失败都变成 `TurnStatus::failed` + `Notice(error)`；调用方只处理一种返回值 |
| 会话写入失败 | 不终止这一轮：发一次 `Notice(error)`「会话记录写入失败，之后的内容不会保存：…」，Recorder 进入停用状态（[09-record](09-record.md)） |

`run_turn` 里只 catch 已知的异常类型（`ModelError`、`session::SessionError`、`mcp::McpError`）；其余异常是编程错误，
让它传出去，不用 `catch (...)` 吞掉。

### 6.2 文本

- 进入 `Message` 的一切文本都必须是合法 UTF-8（nlohmann `dump()` 遇到非法字节会抛）。工具结果 tools 层已处理；
  用户输入（尤其 run 模式的 stdin）和 MCP 名字在进入 Conversation 前过 `base::to_valid_utf8`。
- 核心自己生成、给模型看的所有文字集中在 [03-conversation §5](03-conversation.md) 的「标准文本」表里，不散落在各处。

### 6.3 日志

- `base::logger("agent")`、`base::logger("ui")`。
- info：会话创建/恢复、每一步的模型调用（耗时、tokens、finish）、压缩、MCP 连接结果。
- warn：重试、会话写入失败、工具环境问题、压缩失败退化。
- debug：每个工具调用的名字、摘要、耗时、决策。
- **不记**：请求体、api key、工具输出全文、用户输入全文（可能含密钥）。
- 交互模式下日志不能写 stderr（会弄花界面）：`LogOptions::also_stderr` 只在 run 模式按需打开。

### 6.4 命名

命名空间 `dagent::agent`、`dagent::ui`；库 `dagent_agent`、`dagent_ui`；可执行 `dagent`。

---

## 7. 术语

| 术语 | 含义 |
| --- | --- |
| 轮（turn） | 用户一条输入到 agent 停下，`run_turn` 的一次调用 |
| 步（step） | 一轮里的一次模型请求（不含摘要请求） |
| 批（batch） | 一条 assistant 消息里的全部 tool_calls |
| 并行组 | 一批里连续的、可以一起跑的只读调用 |
| 闭合 | 每个 tool_call 都有对应的 tool 消息（[03-conversation §2](03-conversation.md) 不变式 I1） |
| 会话授权 | 用户选「本会话允许」后记住的规则，只在内存里 |
| 保护区 | 压缩时不动的历史尾部 |
| 切点 | 摘要压缩把历史切成「前缀」和「尾部」的位置 |
| 序号（ordinal） | 会话记录里第几条消息事件（user / assistant / tool），恢复和压缩靠它对齐 |

---

## 8. 里程碑

| 里程碑 | 内容 | 做完之后 |
| --- | --- | --- |
| **C0 准备**（已完成） | 网关实测、验收材料、提示词第一版 | 知道用哪个网关验收 |
| **C1 最小循环**（已完成） | Options 迁移、Model、Conversation、Agent 串行调度、auto/deny 权限、提示词、会话写入、Event、`main` + run 模式 text 输出 | `dagent run "修好这个 bug"` 真的能改代码 |
| **C2 调度与权限**（已完成） | 并行组、调用上限、完整的 Policy 与会话授权、沙箱降级、json/jsonl 输出、退出码、Ctrl+C | 能放进 CI；读多的任务明显变快 |
| **C3 会话** | 回放重建、崩溃闭合、`--resume` / `--continue`、`sessions` / `trust` 子命令 | 会话中断后能接着做 |
| **C4 交互界面** | Shell、Transcript、StatusLine、ApprovalDialog、输入排队、信任询问、恢复时重画 | 日常使用 |
| **C5 上下文管理** | 预算、裁剪、摘要、超长恢复、`/compact` | 长任务不会撞上窗口 |
| **C6 MCP** | McpHub：后台连接、list_changed、断线重连 | 接入外部工具 |

任务拆分、依赖顺序和 20 个验收场景见 [plan.md](plan.md)。C4 和 C5 可以互换：C3 之后如果 run 模式的长任务已经
撞窗口，先做 C5。

---

## 9. 审核流程

1. 每完成一个里程碑，告诉我审核范围（比如「审核 C2」）。
2. 我逐条对照对应文档，重点检查：
   - 接口和约定是否一致；
   - **闭合不变式在所有退出路径上是否成立**（正常、中断、拒绝、上限、异常）；
   - 线程规则（§5）；
   - 密钥、用户输入是否进了日志或错误信息；
   - 正确性缺陷。

   结果按严重程度排序给你。
3. 我复用或重建临时验收材料，跑一遍这个里程碑的验收场景，补上文档里有、检测没覆盖到的。
4. 全部完成后写 `docs/design/agent.md`、`docs/design/ui.md`，更新 `docs/design/app.md` 里「入口待核心组装」的说法和
   [docs/README.md](../README.md) 的状态表，删掉本目录。
