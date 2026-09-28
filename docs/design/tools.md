# tools：工具层

把外围模块包装成模型能调用的工具：定义名字、说明和参数 Schema，解析并校验模型给的参数，调用外围模块，
把结果整理成两份——**给模型的文本**和**给界面与会话的结构化数据（View）**。头文件在 `src/public/tools/`，
实现在 `src/private/tools/`，构建为静态库 `tools`（不带 `dagent_` 前缀），命名空间 `dagent::tools`。
依赖 agent（只用中立契约：`ToolSpec`、`PreparedTool`、`PreparedIntent`、`ExecutionGrant`、`ToolResult`、`ToolSession`）、
base、exec、workspace、mcp。核心不依赖 tools：后端装配把 `tools::ToolSession` 作为核心端口的实现交给 Session。

分工的判断标准和外围相反：这里放「面向模型的语义」。read 要不要带行号、edit 怎样匹配、bash 的输出怎样
呈现给模型，都在这一层；「执行一条命令、截断输出」仍然在 exec。

**不做**：

- 权限决策。工具只**陈述**自己打算做什么（`PreparedIntent`），允许、询问还是拒绝由核心决定。
- 调度。哪些调用并行、调用上限、失败后是否继续，由核心决定。
- 消息历史。结果怎样放进 `agent::Message`、要不要压缩，由核心决定。
- 控制动作。todo / ask / exit_plan / task 的名字、Schema 与说明保持原样，但解析与执行在核心 `agent/control.*`（见 [agent §7](agent.md#控制动作askexit_plantodotask)）。

这些职责的实际接入见 [agent：运行时](agent.md)，展示数据的显示见 [ui](ui.md#5-对话投影)。

`tools/detail.hpp` 放的是各工具实现文件共用的代码（参数解析、路径解析、文本拼装、各工具的工厂函数），
外部不要 include。

---

## 1. 和核心的边界

```
模型给出 ToolCall{id, name, arguments}
  │
  ▼ ActionCatalog：控制动作由核心解析；普通工具交给 ToolSession
  ▼ registry.find(name)            找不到 → 核心自己构造未知工具结果
  ▼ tool.prepare(arguments, ctx)  解析、校验、预演；参数有问题 → 直接得到 is_error 的 ToolResult
  │                                成功 → PreparedTool，带只读的 PreparedIntent（资源、命令意图、diff 预览、摘要）
  ▼ 核心：按 PreparedIntent 做权限决策     拒绝 → 核心构造拒绝结果
  ▼ prepared->execute(grant, on_output, stop)
  ▼ ToolResult{model_text → 模型, display → 界面/记录, signals → 执行信号}
```

**两阶段**：权限对话框需要在执行前看到「要改什么」（edit 的 diff、bash 的命令和只读分析），而且注定失败的
调用（找不到 `old_string`、参数缺字段、超过写入上限、路径不存在）不应该先弹一次确认。`prepare` 没有副作用，
可以读文件，但不写任何东西。调用身份由 Dispatcher 的槽位持有；PreparedTool 只拥有执行所需参数与只读意图。

```cpp
tools::Registry registry;
tools::add_builtin(registry);                        // read/write/edit/bash/grep/glob
tools::add_mcp(registry, client);                    // shared_ptr<mcp::Client>，每个 MCP server 一次
tools::Context ctx(root, config.tools, config.files, config.search, config.process);
tools::ToolSession session(registry, ctx);           // 实现 agent::ToolSession，交给核心 Session

auto prepared = session.prepare(call.name, call.arguments);
if (!prepared) return prepared.error();              // is_error 的 ToolResult
const agent::PreparedIntent& intent = (*prepared)->intent(); // 权限决策的输入
agent::ToolResult result = (*prepared)->execute(grant, on_output, stop);
```

| 类型 | 作用 |
| --- | --- |
| `Options` | `config.json` 的 `tools` 段：给模型的文本上限、read 默认行数与单行上限、grep/glob 数量上限、bash 最长超时、MCP 调用超时 |
| `Spec` = `agent::ToolSpec` | 名字、说明、参数 JSON Schema；工具层与核心共用同一个类型，不再往返复制 |
| `agent::PreparedIntent` | 工具打算做什么：`ToolKind`（read / write / exec / external）、`ResourceIntent`（规范化路径、读写方向、工作区内外）、bash 的 `CommandIntent`（原命令、分析版本、语法状态、dynamic、known_readonly、dangerous、路径/网络影响）、diff 预览和摘要 |
| `Grant` = `agent::ExecutionGrant` | 核心的决定：沙箱 profile、backend、来源、读写/保护范围、通信开关与私有临时空间 |
| `Result` = `agent::ToolResult` | `model_text` 给模型、`is_error`、`interrupted`、`display`（View）、`signals`（如 `McpDisconnected`） |
| `Context` | 会话级状态，每个会话一个，所有调用共用，线程安全；持有工作区根、各模块的 Options 和 FileTracker |
| `Registry` | 名字 → 工具；`specs()` 按注册顺序返回，`retain(names)` 收窄（子 Agent 工具集），`remove_prefix` 移除失效的 MCP 工具 |
| `ToolSession` | 核心 `agent::ToolSession` 端口的实现：Registry + Context |

完整的 shell 分析树（`exec::Analysis`）、MCP Client 与 workspace 解析结果留在具体 PreparedTool 内部；核心只看到上面的中立摘要。
dangerous / known_readonly 仍由 exec 的原分析函数计算，核心不重写白名单。

- **`execute` 不抛异常**：核心对每个调用只需要处理一种返回值。取消返回 `interrupted = true`，`text` 里是已有的
  部分输出；环境问题（rg 没装、沙箱准备失败、MCP 断连）模型修不了，但也应该知道，同样作为 `is_error` 的
  结果返回，同时记 warn 日志。`PreparedTool::execute` 统一包了一层 catch，各工具实现的是 `do_execute`。
- **所有给模型的文本都是合法 UTF-8**：nlohmann 在 `dump()` 遇到非法 UTF-8 会抛 `type_error.316`，整条消息
  就发不出去。rg 输出、bash 输出、MCP 文本都经过 `base::to_valid_utf8`。
- **Spec 的顺序稳定**：DeepSeek 等网关按前缀缓存 prompt，工具列表顺序一变缓存就失效。`Registry::add` 遇到
  同名工具时原位替换；`remove_prefix("mcp__<server>__")` 用于移除断开或失败的 MCP server 工具。
- 工具说明直接写在各 `.cpp` 的原始字符串里，和 Schema 放在一起。

---

## 2. 参数与路径

- `arguments` 是坏 JSON 时返回 `is_error`，带 nlohmann 的报错位置；空串按 `{}` 处理；不是对象也报错。
- 必填字段缺失、类型错 → `is_error`，指出字段名。**宽容一种常见错误**：整数和布尔字段收到字符串 `"10"`、
  `"true"` 时照常接受；未知字段忽略。
- 所有路径参数：相对路径基于 `ctx.root()`，开头的 `~/` 展开成 `$HOME`，统一经 `workspace::resolve`。给模型
  和界面看的路径，在工作区内显示相对路径，在工作区外显示绝对路径。

### FileTracker

`Context` 记录模型读过/写过的文件 → 当时的 `workspace::Stamp`，edit/write 用它做 stale 检测：

- 按 `resolve` 之后的路径做键：`./a`、`a`、指向同一文件的符号链接都算同一个文件。
- read（部分读取也算）、edit、write 成功后更新；连续编辑不需要重新 read。
- 内部加锁：只读调用可能被核心并行执行。

---

## 3. 各工具

### read

参数：`path`，`offset`（从 1 开始的行号，默认 1），`limit`（行数，默认 `read_default_lines`）。

- 输出每行 `行号\t内容`，行号不补空格；单行超过 `read_max_line_bytes` 时在字符边界截断并补 `…`。
- 正文按 `max_result_bytes` 截断，结尾补一行说明：`[文件共 N 行，已显示 a–b 行，用 offset 继续读]`、
  `[文件共 N 行，已全部显示]`，或 offset 超出末尾的提示；空文件输出 `（空文件）`。含非法 UTF-8（`lossy`）、
  文件超过 `files.max_read_bytes` 被截断时，各追加一行说明。
- 二进制：`is_error`，说明大小，不输出内容。不存在：`is_error`，用 `workspace::files` + `fuzzy_rank` 给出
  至多 3 个相近路径。
- **目录**：列出直接子项（目录名带 `/`，至多 `glob_max_files` 项，同样按 `max_result_bytes` 截断），省掉单独的
  ls 工具。
- CRLF 和 BOM 由 `workspace::read_text` 处理，输出里没有 `\r` 和 BOM。
- display：`ReadView`——界面只显示「读取 a.cpp 1–200 行」，不显示内容。

### edit

参数：`path`，`old_string`，`new_string`，`replace_all`（默认 false）。

- **必须先 read**：FileTracker 里没有这个文件 → `is_error`「请先用 read 读这个文件再编辑」。
- **stale**：`prepare` 时当前 Stamp 和 FileTracker 里的不一致 → `is_error`，提示文件在上次读取后被修改过
  （可能是 bash 或用户改的），请重新 read。`execute` 写入时再用 `write_text(expect=…)` 兜底，覆盖 prepare 到
  execute 之间用户确认的那段时间。
- 匹配在 LF 内容上做，`old_string`/`new_string` 先统一成 LF；写回时保留原文件的 eol 和 BOM。
- **只做精确匹配，并且必须唯一**：0 处 → `is_error`；多处且没开 `replace_all` → `is_error`，列出各处的行号
  （至多 20 个）。`old_string` 为空、`old_string == new_string`、替换后内容不变 → `is_error`。
- 失败提示要让模型能自己修好：`old_string` 每行都以「数字 + Tab」开头时，提示不要带 read 输出的行号前缀；
  忽略行首缩进后能唯一匹配时，指出行号并提示缩进不一致——只提示，不自动应用。
- **拒绝编辑**：二进制文件；`lossy` 的文件（替换过 U+FFFD 的内容写回去会破坏原字节）；超过 `max_read_bytes`
  被截断读取的文件（写回会丢掉后面的内容）；编辑后超过 `files.max_write_bytes` 的文件。这些都在 `prepare` 里
  拦下，不会等用户确认之后才失败。
- `prepare` 里算好新内容和 `unified_diff`，放进意图的 diff 预览；`execute` 只负责写。
- 成功后给模型：`已编辑 path（+a −b）`，加上每处改动前后各 4 行（带行号），多处时总量按预算截断并说明
  另有几处未展示。
- display：`FileChangeView`（`created = false`）。

### write

参数：`path`，`content`。

- 文件已存在：同 edit，必须先 read、做 stale 检查，保留原文件的 eol 和 BOM；二进制文件拒绝覆盖。
  文件不存在：直接创建（父目录自动建），LF、无 BOM。`content` 先统一成 LF。
- 超过 `files.max_write_bytes` → `is_error`（在 prepare 里判断）。
- 给模型：`已创建 path（N 行）` / `已覆盖 path（+a −b）`，不回显内容。
- display：`FileChangeView`。

### bash

参数：`command`，`timeout_ms`（可选，上限 `bash_max_timeout`，不给时用 `process.default_timeout`）。

- 执行 `bash -c <command>`，cwd 是 `ctx.root()`，stderr 合并进 stdout，stdin 接 `/dev/null`，
  `LC_ALL=C.UTF-8`（报错文本不随系统 locale 变化，同时 `ls` 不会把中文文件名转义成八进制）。
- **每次调用都是新进程**：`cd`、`export` 不保留。说明里告诉模型需要换目录就写 `cd dir && …`；后台常驻进程
  （`server &`）会在主进程退出后被清理，说明里写明不支持。
- **沙箱**：Grant 的 profile 不是 `full_access` 时把其明确读写范围、受保护路径、通信开关与私有临时空间要求
  原样交给 `exec::prepare`；准备失败时命令不执行，返回 `is_error`，没有 full_access 回退。
- **意图**：prepare 只调用一次 `exec::analyze`，完整 `Analysis` 留在 PreparedTool 内供执行复用，向核心公开
  `CommandIntent` 摘要（含 known_readonly / dangerous）；策略与审批只看摘要。语法错误在 prepare 返回带字节位置的未执行错误。
- **给模型的文本**：`strip_ansi` → `to_valid_utf8` → `truncate_middle`，正文预算是 `max_result_bytes` 减去
  512 字节的余量，**截断之后**再追加状态行，保证状态行不会被切掉、整体仍在预算内：
  - 退出码非 0 时 `[退出码 N]`，超时 `[超时，已在 Ns 后终止]`，信号 `[被信号 N 终止]`，取消 `[已被用户中断]`，
    启动失败或执行层失败 `[无法执行命令：…]` / `[执行失败：…]`；没有输出时写 `（无输出）`；退出码 0 不写状态行。
  - 普通非零退出只按退出码/信号/超时报告；不会仅凭 stderr 关键词声称是沙箱拒绝或自动请求扩权。
- `is_error`：退出码非 0、被信号终止、超时、无法执行。
- `on_output` 把原始输出块交给核心，核心 `post` 给界面。
- display：`BashView` 额外保存实际 backend、grant source 与 analysis version；`output` 是 exec 按 `process.max_output_bytes` 截断后的输出，不是给模型的那份，
  存进会话时直接进入 SQLite payload BLOB。

### grep

参数：`pattern`，`path`（目录或单个文件，默认工作区根），`glob`，`type`，`ignore_case`，`context`（至多 10），
`files_only`。

- 调 `workspace::grep`，`max_matches = grep_max_matches`；`path` 不存在时在 prepare 里报错。
- 输出沿用 rg 默认格式，模型最熟悉：匹配行 `path:line:text`，上下文行 `path-line-text`，不连续的组之间用
  `--` 分隔；`files_only` 时每行一个路径。路径都**相对工作区根**，模型可以直接拿去 read。没有匹配时输出
  `（无匹配）`；截断时补一行 `[结果已截断，请缩小范围]`。
- 正则错误 → `is_error`，带 rg 的报错原文；这是模型的输入错误，不记 warn。
- display：`GrepView`（`files_only` 时 `lines` 里只有 `path`）。

### glob

参数：`pattern`，`path`（目录，默认工作区根）。

- 调 `workspace::files`，`sort_by_mtime = true`，`max_files = glob_max_files`；每行一个路径，相对工作区根。
- rg 的 `--glob` 是 **gitignore 语义**，不是 shell glob：`*.cpp` 匹配任意深度。说明里写清楚了，并给出
  `src/**/*.hpp` 这样的例子。被 `.gitignore` 忽略的文件和隐藏文件不列出。
- `path` 不存在或不是目录时在 prepare 里报错（文件请用 read）。
- display：`GlobView`。

### todo / ask / exit_plan / task

这四个名字仍出现在模型的工具列表里（顺序 read、write、edit、bash、grep、glob、todo、ask、exit_plan，主会话再加 task），
但它们是核心控制动作，不在工具层注册。Schema、说明、给模型的文本与 View（TodoView / AskView / TaskView）保持原样；
解析与执行规则见 [agent §7](agent.md#控制动作askexit_plantodotask) 与 [agent §12](agent.md#12-子-agent-与-task)。

### MCP 工具

`add_mcp` 把 `mcp::Client::tools()` 里的每一项包成一个 Tool：名字用 `qualified_name`，Schema 用
`input_schema`，说明为空时补一句「MCP server「x」提供的工具 y」；意图为 `external`。工具项持有 Client 的 `shared_ptr`。

- 调用超时是 `Options::mcp_call_timeout`（默认 120 秒）。
- 结果转文本：`text` 块直接拼接；`image`/`audio` 块写成 `[图片 image/png，N 字节，未展示]`；`resource` 块
  写 uri，带文本就附上；`resource_link` 写名字和 uri；没有 `content` 只有 `structured` 时输出紧凑 JSON。
  整体按 `max_result_bytes` 截断。`is_error` 沿用 `isError`。
- `McpError::cancelled` 返回 `interrupted`；其他 `McpError` 都转成 `is_error` 的结果并记 warn，
  `disconnected` 时置 `McpView::disconnected` 并附执行信号 `McpDisconnected{server}`，核心据此交给 Hub 标记断开。
- 工具项持有 Client 的 `shared_ptr`：Hub 重连某个 server 前移除旧工具，连接成功后通过 `add_mcp` 合并新工具，
  仍被子会话快照引用的旧连接不会悬空。
- display：`McpView`，内容块原样保留，界面自己决定怎样显示。

### McpHub

`tools::McpHub`（`tools/mcp_hub.hpp`）管理配置的全部 MCP server：构造时为每个 server 启动后台连接；`apply_pending`
在主会话的模型步骤之间处理重连、等待与工具合并；`snapshot` 把已就绪工具合并进子会话的注册表；`mark_disconnected`
返回追加给模型的 T12 / T13；`states` 线程安全地给出状态快照。生命周期规则见 [agent §11](agent.md#11-mcp-生命周期)。

连接只接受 MCP `2026-07-28`，通过 `server/discover` 确认版本，不回退旧协议。Hub 在连接或重连后合并工具，
不接收服务端工具变更通知，也不周期刷新工具列表。

---

## 4. 界面数据：View

`ToolResult::display` 给界面用，也存进会话记录，供历史显示。结构体在核心 `agent/tool_data.hpp`，工具层与控制动作共用：

| View | 字段 |
| --- | --- |
| `ReadView` | `path`、`start_line`、`end_line`、`total_lines`、`truncated`、`directory` |
| `FileChangeView` | `path`、`diff`（unified diff 文本）、`added`、`removed`、`created`；edit 和 write 共用 |
| `BashView` | `command`、`output`、退出状态、实际 backend/profile、grant source、analysis version、读写/保护范围、敏感名称规则、network/local sockets/private tmp、`elapsed_ms` |
| `GrepView` | `pattern`、`lines`（`GrepLine`：`path`、`text`、`line`、`spans`、`is_context`）、`truncated` |
| `GlobView` | `pattern`、`files`、`truncated` |
| `McpView` | `server`、`tool`、`content`、`structured`、`disconnected` |
| `TodoView` | `items` 整份列表；`TodoItem` 含 `text` 与四态 `state` |
| `TaskView` | `agent`、`task`、`session_id`（父到子的跳转锚点）、`result`、`steps`（`TaskStep`：`summary`、`is_error`）、`model_calls`、`tool_calls`、`seconds`、`interrupted` |

- `View = std::variant<std::monostate, ReadView, FileChangeView, BashView, GrepView, GlobView, McpView, TodoView, AskView, TaskView>`；
  `monostate` 表示 prepare 阶段就失败的调用（参数错误等），界面只显示文本。前端不链接核心，
  由 `ui/projection` 把协议里的 View JSON 解码成自己的投影类型，未知 kind 按文本回退。
- `agent::to_json(view)` 输出 `{"kind": ..., 各字段}`，写进 tool 记录的 `view`，协议也原样传给前端；`kind` 为 `read`、`change`、
  `bash`、`grep`、`glob`、`mcp`、`todo`、`ask`、`task`，`monostate` 使用显式 `kind: null`。
- `view_from_json` 严格读取当前完整字段；未知 `kind`、缺失字段、非法字段类型或损坏条目均报错。
  不为旧会话补默认字段，也不把损坏展示数据转换成 `monostate`。

---

## 5. 对核心的要求

这几条在工具层做不了，只能由核心保证：

- **每个 tool_call 都要有一条 tool 消息**，包括未知工具、参数错误、用户拒绝、被取消的调用；否则 OpenAI
  协议下一次请求直接 400。顺序与 `tool_calls` 一致。
- 可以并行的只有 read 意图和获准 `read_only` 沙箱的 bash；write、edit、MCP 串行，`task` 单独成组并发。
- FileTracker 跟着会话走；恢复会话时不重建，模型需要重新 read 才能编辑——宁可多读一次，也不能拿旧 Stamp
  覆盖别人的改动。
- read 可以读到 `.env` 这类文件，内容会发给模型。是否拦截由核心的权限策略决定（意图里有路径）。

---

## 6. 已知限制

- 没有多处编辑（`edits` 数组）、后台 shell、web_fetch、图片读取（编解码器还只支持文本）。
- edit 只做精确匹配，失败时给提示，不做模糊回退。
- bash 每次都是新进程，不保留 cwd。以后如果要保留，要先考虑和并行执行的冲突。
- write 新建文件时不检查 prepare 之后是否有别人抢先创建了同名文件。

---

## 7. 配置与构建

- 选项对应 `home/config/config.json` 的 `tools` 段：`max_result_bytes`（32 KiB，约 8k token）、
  `read_default_lines`（2000）、`read_max_line_bytes`（2000）、`grep_max_matches`（200）、`glob_max_files`（200）、
  `bash_max_timeout_ms`（600000）、`mcp_call_timeout_ms`（120000），由 app 映射。文件读写上限沿用 `files` 段：
  `max_read_bytes` 是 read 能翻页的最大文件（8 MiB），`max_write_bytes` 是写入上限（1 MiB）。
- 需要模型的真实功能检测使用当前开发配置的本地 Qwen3.8-Flash-Next，支持工具调用；接入方式见
  [文档索引](../README.md)。临时检测材料只放在 `temp/`。
