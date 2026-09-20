# tools：工具层

把外围模块包装成模型能调用的工具：定义名字、说明和参数 Schema，解析并校验模型给的参数，调用外围模块，
把结果整理成两份——**给模型的文本**和**给界面与会话的结构化数据（View）**。头文件在 `src/public/tools/`，
实现在 `src/private/tools/`，构建为静态库 `tools`（不带 `dagent_` 前缀），命名空间 `dagent::tools`。
依赖 base、exec、workspace、mcp；**不依赖 agent**（核心依赖它，反过来会成环）。

分工的判断标准和外围相反：这里放「面向模型的语义」。read 要不要带行号、edit 怎样匹配、bash 的输出怎样
呈现给模型，都在这一层；「执行一条命令、截断输出」仍然在 exec。

**不做**：

- 权限决策。工具只**陈述**自己打算做什么（`Intent`），允许、询问还是拒绝由核心决定。
- 调度。哪些调用并行、调用上限、失败后是否继续，由核心决定。
- 消息历史。结果怎样放进 `agent::Message`、要不要压缩，由核心决定。

这些职责的实际接入见 [agent：运行时](agent.md)，View 的显示见 [ui](ui.md#5-event-到对话文档)。

`tools/detail.hpp` 放的是各工具实现文件共用的代码（参数解析、路径解析、文本拼装、各工具的工厂函数），
外部不要 include。

---

## 1. 和核心的边界

```
模型给出 ToolCall{id, name, arguments}
  │
  ▼ registry.find(name)           找不到 → 核心自己构造错误结果
  ▼ tool.prepare(arguments, ctx)  解析、校验、预演；参数有问题 → 直接得到 is_error 的 Result
  │                               成功 → Call，带 Intent（路径、命令、diff 预览……）
  ▼ 核心：按 Intent 做权限决策     拒绝 → 核心构造「用户拒绝」结果
  ▼ call.run(grant, on_output, stop)
  ▼ Result{text → 模型, display → 界面/会话}
```

**两阶段**：权限对话框需要在执行前看到「要改什么」（edit 的 diff、bash 的命令和只读分析），而且注定失败的
调用（找不到 `old_string`、参数缺字段、超过写入上限、路径不存在）不应该先弹一次确认。`prepare` 没有副作用，
可以读文件，但不写任何东西。

```cpp
tools::Registry registry;
tools::add_builtin(registry);                       // read / write / edit / bash / grep / glob
tools::add_mcp(registry, *client);                  // 每个 MCP server 一次
tools::Context ctx(root, config.tools, config.files, config.search, config.process);

for (const tools::Spec* spec : registry.specs()) { /* 转成 agent::ToolDef */ }

const tools::Tool* tool = registry.find(call.name);
auto prepared = tool->prepare(call.arguments, ctx);
if (!prepared) return prepared.error();              // is_error 的 Result
const tools::Intent& intent = (*prepared)->intent(); // 权限决策的输入
tools::Result result = (*prepared)->run(grant, on_output, stop);
```

| 类型 | 作用 |
| --- | --- |
| `Options` | `dagent.json` 的 `tools` 段：给模型的文本上限、read 默认行数与单行上限、grep/glob 数量上限、bash 最长超时、MCP 调用超时 |
| `Spec` | 名字、说明、参数 JSON Schema；核心把它一一对应地转成 `agent::ToolDef` |
| `Intent` | 工具打算做什么：`kind`（read / write / exec / external）、涉及的路径（带 `inside_workspace`）、bash 命令与 `known_readonly`、edit/write 的 unified diff 预览、一行摘要 |
| `Grant` | 核心的决定：bash 的沙箱模式与是否允许联网 |
| `Result` | `text` 给模型、`is_error`、`interrupted`、`display`（View） |
| `Context` | 会话级状态，核心每个会话建一个，所有调用共用，线程安全；持有工作区根、各模块的 Options 和 FileTracker |
| `Registry` | 名字 → 工具；`specs()` 按注册顺序返回 |

- **`run` 不抛异常**：核心对每个调用只需要处理一种返回值。取消返回 `interrupted = true`，`text` 里是已有的
  部分输出；环境问题（rg 没装、沙箱准备失败、MCP 断连）模型修不了，但也应该知道，同样作为 `is_error` 的
  结果返回，同时记 warn 日志。`Call::run` 统一包了一层 catch，各工具实现的是私有的 `do_run`。
- **所有给模型的文本都是合法 UTF-8**：nlohmann 在 `dump()` 遇到非法 UTF-8 会抛 `type_error.316`，整条消息
  就发不出去。rg 输出、bash 输出、MCP 文本都经过 `base::to_valid_utf8`。
- **Spec 的顺序稳定**：DeepSeek 等网关按前缀缓存 prompt，工具列表顺序一变缓存就失效。`Registry::add` 遇到
  同名工具时原位替换；`remove_prefix("mcp__<server>__")` 用于刷新某个 MCP server 的工具。
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
  （可能是 bash 或用户改的），请重新 read。`run` 写入时再用 `write_text(expect=…)` 兜底，覆盖 prepare 到
  run 之间用户确认的那段时间。
- 匹配在 LF 内容上做，`old_string`/`new_string` 先统一成 LF；写回时保留原文件的 eol 和 BOM。
- **只做精确匹配，并且必须唯一**：0 处 → `is_error`；多处且没开 `replace_all` → `is_error`，列出各处的行号
  （至多 20 个）。`old_string` 为空、`old_string == new_string`、替换后内容不变 → `is_error`。
- 失败提示要让模型能自己修好：`old_string` 每行都以「数字 + Tab」开头时，提示不要带 read 输出的行号前缀；
  忽略行首缩进后能唯一匹配时，指出行号并提示缩进不一致——只提示，不自动应用。
- **拒绝编辑**：二进制文件；`lossy` 的文件（替换过 U+FFFD 的内容写回去会破坏原字节）；超过 `max_read_bytes`
  被截断读取的文件（写回会丢掉后面的内容）；编辑后超过 `files.max_write_bytes` 的文件。这些都在 `prepare` 里
  拦下，不会等用户确认之后才失败。
- `prepare` 里算好新内容和 `unified_diff`，放进 `Intent::preview`；`run` 只负责写。
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
- **沙箱**：`Grant::sandbox` 不是 `full_access` 时调 `exec::prepare`，`writable = {root, /tmp}`；准备失败时
  命令不执行，返回 `is_error`。
- **Intent**：`exec::analyze` 后填 `known_readonly`。只读判断只是给核心的依据，核心自动放行只读命令时仍然
  应该给 `read_only` 沙箱（见 [exec 设计文档](exec.md)）。
- **给模型的文本**：`strip_ansi` → `to_valid_utf8` → `truncate_middle`，正文预算是 `max_result_bytes` 减去
  512 字节的余量，**截断之后**再追加状态行，保证状态行不会被切掉、整体仍在预算内：
  - 退出码非 0 时 `[退出码 N]`，超时 `[超时，已在 Ns 后终止]`，信号 `[被信号 N 终止]`，取消 `[已被用户中断]`，
    启动失败或执行层失败 `[无法执行命令：…]` / `[执行失败：…]`；没有输出时写 `（无输出）`；退出码 0 不写状态行。
  - 在沙箱里**失败**、且输出里有 `Permission denied`、`Read-only file system`、`Operation not permitted`、
    `Could not resolve host` 这类字样时，再追加一行沙箱提示，让模型换个做法或向用户说明，而不是反复重试。
- `is_error`：退出码非 0、被信号终止、超时、无法执行。
- `on_output` 把原始输出块交给核心，核心 `post` 给界面。
- display：`BashView`；`output` 是 exec 按 `process.max_output_bytes` 截断后的输出，不是给模型的那份，
  存进会话时会转成 blob。

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

### MCP 工具

`add_mcp` 把 `mcp::Client::tools()` 里的每一项包成一个 Tool：名字用 `qualified_name`，Schema 用
`input_schema`，说明为空时补一句「MCP server「x」提供的工具 y」；Intent 为 `external`。

- 调用超时是 `Options::mcp_call_timeout`（默认 120 秒）。
- 结果转文本：`text` 块直接拼接；`image`/`audio` 块写成 `[图片 image/png，N 字节，未展示]`；`resource` 块
  写 uri，带文本就附上；`resource_link` 写名字和 uri；没有 `content` 只有 `structured` 时输出紧凑 JSON。
  整体按 `max_result_bytes` 截断。`is_error` 沿用 `isError`。
- `McpError::cancelled` 返回 `interrupted`；其他 `McpError` 都转成 `is_error` 的结果并记 warn，
  `disconnected` 时置 `McpView::disconnected`，核心据此决定是否重新 connect。
- `mcp::Client` 必须比这些工具活得久；`refresh_tools` 之后核心先 `remove_prefix("mcp__<server>__")` 再
  `add_mcp`。
- display：`McpView`，内容块原样保留，界面自己决定怎样显示。

---

## 4. 界面数据：View

`Result::display` 给界面用，也存进会话，供 `--resume` 时重画历史。每个工具一个结构体，在 `tools/view.hpp`：

| View | 字段 |
| --- | --- |
| `ReadView` | `path`、`start_line`、`end_line`、`total_lines`、`truncated`、`directory` |
| `FileChangeView` | `path`、`diff`（unified diff 文本）、`added`、`removed`、`created`；edit 和 write 共用 |
| `BashView` | `command`、`output`、`exit_code`、`signal`、`timed_out`、`interrupted`、`sandbox`（`read_only` / `workspace_write` / `full_access`）、`allow_network`、`elapsed_ms` |
| `GrepView` | `pattern`、`lines`（`GrepLine`：`path`、`text`、`line`、`spans`、`is_context`）、`truncated` |
| `GlobView` | `pattern`、`files`、`truncated` |
| `McpView` | `server`、`tool`、`content`、`structured`、`disconnected` |

- `View = std::variant<std::monostate, ReadView, FileChangeView, BashView, GrepView, GlobView, McpView>`；
  `monostate` 表示 prepare 阶段就失败的调用（参数错误等），界面只显示 `text`。界面用 `std::visit` 处理，
  漏掉哪种 View 编译时就能发现。
- `to_json(view)` 输出 `{"kind": ..., 各字段}`，给 `session::Writer::append`；`kind` 为 `read`、`change`、
  `bash`、`grep`、`glob`、`mcp`，`monostate` 为 `null`。`view_from_json` 在 `kind` 不认识或条目损坏时返回
  `monostate`。
- 反序列化时缺字段取默认值：以后给结构体加字段，旧会话照样读得出来。

---

## 5. 对核心的要求

这几条在工具层做不了，只能由核心保证：

- **每个 tool_call 都要有一条 tool 消息**，包括未知工具、参数错误、用户拒绝、被取消的调用；否则 OpenAI
  协议下一次请求直接 400。顺序与 `tool_calls` 一致。
- 可以并行的只有 `Intent::Kind::read` 和 `known_readonly` 的 bash；write、edit、MCP 串行。
- FileTracker 跟着会话走；恢复会话时不重建，模型需要重新 read 才能编辑——宁可多读一次，也不能拿旧 Stamp
  覆盖别人的改动。
- read 可以读到 `.env` 这类文件，内容会发给模型。是否拦截由核心的权限策略决定（Intent 里有路径）。

---

## 6. 已知限制

- 没有多处编辑（`edits` 数组）、后台 shell、web_fetch、todo、图片读取（编解码器还只支持文本）。
- edit 只做精确匹配，失败时给提示，不做模糊回退。
- bash 每次都是新进程，不保留 cwd。以后如果要保留，要先考虑和并行执行的冲突。
- write 新建文件时不检查 prepare 之后是否有别人抢先创建了同名文件。

---

## 7. 配置与构建

- 选项对应 `config/dagent.json` 的 `tools` 段：`max_result_bytes`（32 KiB，约 8k token）、
  `read_default_lines`（2000）、`read_max_line_bytes`（2000）、`grep_max_matches`（200）、`glob_max_files`（200）、
  `bash_max_timeout_ms`（600000）、`mcp_call_timeout_ms`（120000），由 app 映射。文件读写上限沿用 `files` 段：
  `max_read_bytes` 是 read 能翻页的最大文件（8 MiB），`max_write_bytes` 是写入上限（1 MiB）。
- 需要模型的真实功能检测使用当前开发配置的本地 Qwen3.8-Flash-Next，支持工具调用；接入方式见
  [文档索引](../README.md)。临时检测材料只放在 `temp/`。
