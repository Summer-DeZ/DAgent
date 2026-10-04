# ui：应用层交互界面

`dagent` 不带子命令时进入全屏终端界面。公开头文件在 `src/public/ui/`，实现在 `src/private/ui/`，
库为 `dagent_ui`，只依赖 client、protocol、tui 与 base。界面运行在前端进程，只持有 `client::Client`、协议 DTO
与页面状态：业务操作都经 [私有协议](protocol.md) 发给自己的后端，执行状态、队列、模式与模型的真值都在后端 runtime。
它只使用冻结的 [TUI 框架](tui-framework.md)，没有为应用视觉增加框架原语。

界面 chrome 使用英文，统一来自 `ui/strings.hpp` 的 `Strings` 和 `ui/strings.cpp` 的常量表。
`text()` 只返回英文表的常量引用；字段全为 `string_view`，格式串使用显式参数索引，由 `format_text()` 格式化。
暂不提供语言配置、运行时切换或外部文案文件。工具结果、模型提示词、日志和 CLI 不进入此表；
模型正文和用户提供的计划标题仍跟随用户语言，旧会话的历史文本不迁移。

## 1. 组成与控件树

公开入口 `run_interactive(Client&, FrontendBridge&, InteractiveOptions, stop_token)` 在后端已完成 initialize 后调用：
加载 initialize 返回的主题文件，按初始快照建立页面，恢复启动时经 `session.history` 分页取回并回放历史，然后进入内部 `Shell`。
`FrontendBridge` 把 Client 读线程上的事件、交互请求和断连转交到渲染线程；Shell 创建前到达的通知直接丢弃（此时尚无执行）。
主要部件如下。

| 部件 | 职责 |
| --- | --- |
| `Shell` | 控件寿命、命令映射到 RPC、事件分派、快照/generation 跟踪、页面状态和退出 |
| `Transcript` | 协议事件与历史条目到 Document 块的投影：`apply_live` 处理实时事件，`append_history` 回放历史且不触发当前 Run 收尾 |
| `PromptBox`、`PromptInput` | 自动折行、按行数增高的多行输入；左侧竖条与框内尾行；发送、排队和取回 |
| `StatusLine` | 底部一行：项目路径、上下文用量与命令面板提示 |
| `Completion` | `/` 命令、`@` 文件和 `$` 技能的贴输入框补全浮层 |
| `Panel` | 命令、会话、主题和帮助共用的居中列表浮层 |
| `ToastStack` | 右上角最多三条、五秒到期的瞬时通知 |
| `SidePanel` | 右侧常驻信息栏：会话标题、上下文用量、MCP、最近一份 `TodoView`、项目与版本 |
| `ApprovalDialog` | 权限审批与选项提问共用的模态骨架与输入；回答经 `interaction.answer` 提交 |
| `projection` | 协议 DTO / View JSON → UI 实际消费的字段；子事件由 Shell 按协议信封路由，解码后直接投影，不重复包装身份 |

```text
LayerStack
└─ Container(vertical)
   ├─ Container(horizontal)             主区 + 计划栏
   │  ├─ Container(vertical)            主区
   │  │  ├─ Center(Scrollback)          flex 1，内容栏最大 88 列
   │  │  ├─ Center(Activity)            content，空闲高度 0
   │  │  ├─ Center(QueueLabel)          content，最多 3 行
   │  │  └─ Center(PromptBox)           content，1–8 行正文 + 3 行留白与尾行
   │  └─ SidePanel                      content，26/30 列或宽 0
   └─ StatusLine                        fixed 1，铺满终端
浮层：Completion、Panel、ApprovalDialog、ToastStack
```

`centered()` 在主区至少留四列边距，内容栏最大 88 列；主区窄于 44 列时取消边距。
对话、活动、排队和输入使用同一宽度与左缘。侧栏被收起或终端少于 80 列时宽度为 0；
80–99 列占 26 列，至少 100 列占 30 列。状态栏始终在计划栏之外铺满。

新会话的第一个 Document 块是欢迎头，包含版本、项目、git 分支和 MCP 数量；内容栏少于 60 列时只画单行字标。
历史 `turn_end` 保留在协议与存储中，UI 不将其转换为无人消费的结束事件。
文本显示宽度和带省略号的裁剪统一使用 `ui/display`；工具折叠提示的硬裁剪保留原行为。
恢复会话改画恢复 id 与回放事件数。Scrollback 不贴底时，右下角覆盖显示未读行数，End 回到底部。

## 2. 主题

`ui.theme_file` 指向 JSON 主题；文件在内置 dark / light 令牌上覆盖同名项，支持顶层 `name`、`defs`、
`dark` 和 `light`。默认文件为 `home/themes/dagent.json`，启动时加载，不扫描同目录的其他文件。

Shell 的所有主题变更都经过 `apply_theme()`：复制令牌、递增 `epoch`，再依次刷新 Scrollback、Activity、
排队行、PromptBox、StatusLine、SidePanel、ToastStack、Panel、模型表单、ApprovalDialog 和 Completion。
`/theme` 面板只提供 `dark`、`light`、`follow terminal`：均使用当前配置文件的明暗配色，无文件时才使用
应用层后备色。启动默认跟随终端；打开时选中已确认模式，不触发预览。移动选择使用已加载令牌即时预览，
Enter 保留，Esc / Ctrl+C 恢复已确认模式。预览期间暂停终端背景驱动的切换；退出预览后，跟随模式仍会
响应终端背景变化，手动 dark / light 不受影响。模式选择仅在当前进程生效。
内置明暗主题显式设置底色和正文前景；普通文字令牌继承主题底色，状态栏、面板、toast 和计划栏继承各自
panel 背景，不在底纹上打孔；底部状态栏沿用主背景。切换时使整个控件树失效，两侧留白也使用新底色。

视觉角色只使用 `ThemeTokens`：正文 `text`，次要信息 `text_muted`，用户竖条与选中项 `primary`，
进行中与自动编辑 `accent`，工具完成/失败分别用 `success`/`error`，上下文阈值和重试用 `warning`。
用户消息、输入框和选中行用 `background_element`；侧栏和浮层用 `background_panel`。

默认配色依据 2026-09-20 调研时 OpenCode 1.18.31 的 `orng` 主题，采用中性黑灰/暖白底色和橙色强调。
来源、语义映射和适配取舍见 [OpenCode 主题分析](opencode-theme.md)。256 色终端按 xterm 实际色阶比较
色立方与灰阶的距离，保留深色背景层次，并正确处理纯黑、纯白；真彩终端直接输出 RGB。

## 3. 线程与生命周期

控件、Document、浮层和事件处理器只由渲染线程访问。前端没有业务工作线程：

- 输入、命令、权限切换、回答等发 RPC；同步等待只用于启动阶段，运行中都用 `call_async`，结果 `post` 回渲染线程。
- 后端事件由 Client 读线程经 `FrontendBridge` post 到渲染线程；`CallbackGate` 保证 Shell 析构后在途回调不再触碰页面。
- Shell 记录当前 session_id 与 generation，丢弃旧 generation 的事件；`session.changed` 后取 `session.snapshot` 刷新标签。
  new/resume 时清空投影并用 `session.history` 分页回放；切模型保留 Transcript，只更新标签与上下文。
- 列表、历史、项目信息与文件补全都是后端查询；主会话与子 Pane 各有一个历史分页器，关闭页面时发 `session.history_close`。

Shell 进入全屏后调用 `Terminal::set_mouse(true)` 打开鼠标上报，ScrollbackMouse 才收得到滚轮、
拖选与双击；退出时的终端还原由框架处理。

活动动画忙时每 100 ms 更新；MCP 状态来自会话快照：只在连接中或轮次忙碌时每 200 ms 请求一次 `session.snapshot`，稳定空闲后停止。
toast 使用一次性五秒定时器，文件补全使用一次性 80 ms 防抖；空闲时没有新增的周期唤醒。

## 4. 输入、补全与排队

PromptBox 左侧是一根竖条（忙碌时换成 `primary`），底纹用 `background_element`，正文上方留一行、
与框内最后一行之间再留一行，输入时不贴着上面的对话；最后一行显示 `权限模式 · 模型`。正文按内容栏宽度折行，控件高度跟着折行结果在 1–8 行之间变化（加上三行留白与尾行，整体 4–11 行），超过 8 行在框内滚动并保证
光标可见；空输入显示占位文字，占位文字不属于 `text()`。

新会话 Banner 只显示版本、项目路径、git 分支与 MCP 数量，不重复显示模型；当前模型保留在输入框尾行和消息尾行。

框架的 `tui::InputBox` 只做横向滚动、不折行，所以 PromptBox 自绘正文，不调用基类 `render`；
基类内部的横竖滚动量因此恒为 0，`cursor()` 借这一点从基类光标反推逻辑行号与显示列（基类没有公开它们）。

补全列表的名称和描述使用统一列起点，按字素的终端显示宽度计算列宽；中文、宽字符和窄屏截断不以 UTF-8
字节数计宽，长内容用省略号收尾，避免侵入右侧边框。

普通输入经 `input.submit` 进入后端内存 FIFO，忙时也立即提交；QueueLabel 按快照里的队列显示数量与最近两条的首行预览。
输入框为空时 ↑ 发 `input.recall_last`，把最后一条仍排队的输入取回输入框。后端在 `turn_ended` 或压缩完成后自行处理下一条。
斜杠命令不进队列，立即分派：业务命令（new、sessions、model、compact、plan、permissions）按原 busy 条件发请求，
本地视图命令（theme、help、agents、exit、折叠与侧栏）在前端执行。

| 按键 | 行为 |
| --- | --- |
| Enter | 发送；忙时排队；浮层打开时确认选中项 |
| Shift/Alt+Enter | 输入换行 |
| Esc | Interrupt turn（先关闭补全/面板；否则中断当前轮） |
| Ctrl+C | Clear or exit（先关浮层；忙时中断；有输入时清空；空闲连续两次退出） |
| Shift+Tab | 按 ask → workspace → unrestricted → ask 循环权限模式；unrestricted 用 error 色警示 |
| Ctrl+G | 进入或退出 plan 模式 |
| Ctrl+O | Expand tool output（已完成工具输出展开/折叠） |
| Ctrl+R | Expand thoughts（全部思考展开/折叠） |
| Ctrl+T | 收起或展开右侧信息栏 |
| Ctrl+A | Switch agent view（主会话与子 Agent Pane 切换） |
| Ctrl+P | Command palette（命令面板） |
| Ctrl+? | Keys and commands（帮助面板） |
| PgUp/PgDn、Home/End | Page up/down、Go to top/bottom（翻页、回顶、回底） |

输入是单行且以 `/` 开头时，Completion 从 Shell 注册的命令表按前缀过滤。Tab 只补全，Enter 直接执行，
不把命令提交给模型。命令表同时驱动 ctrl+p 与帮助面板，避免维护第二份标题和分类。

光标前最后一个非空白串以 `@` 开头时打开文件补全。80 ms 防抖后发 `workspace.complete`，后端用 `workspace::files`
按 `ui.completion_max_files` 列举当前项目文件，再用 `fuzzy_rank` 取前八项；每次请求带 Completion 代次，过期结果被丢弃。Enter 用仓库相对路径替换 `@式` 并补空格。

| 命令 | 行为 |
| --- | --- |
| `/new` | New session（清空对话和计划） |
| `/compact` | Compact context（手动压缩，可取消） |
| `/sessions` | Switch session（异步列出并恢复当前 cwd 的会话） |
| `/agents` | Switch agent view（列出主会话与全部 task；恢复出来的子会话在选中时才回放） |
| `/model` | 选择或添加模型；仅空闲时可用 |
| `/permissions` | 查看当前会话授权，Enter 撤销；运行中也可用 |
| `/skills` | 浏览技能和发现诊断，选择后插入 `$name` |
| `/plan` | 进入或退出只读规划模式；新会话不继承 plan |
| `/theme` | Switch theme（即时预览并切换） |
| `/help` | Keys and commands（打开只读帮助） |
| `/exit` | Exit（退出） |

## 5. 对话投影

每个逻辑块左侧有两列装订线；窄内容栏压成一列。装饰符、底纹补空格、工具右侧统计使用 `k_no_src`，
拖选仍得到原始用户输入、Markdown 或工具主体。普通 Markdown、代码、表格和工具主体在原渲染器外套一层 gutter，
折行宽度与实际绘制宽度一致。

| meta / Event | 显示 |
| --- | --- |
| `banner` | 新会话字标、项目信息和提示；恢复使用单行系统块 |
| `turn_started` / 历史 user | `▌` 竖条、正文和整行 `background_element` |
| `TextDelta` | `MarkdownStream`，正文前留两列 |
| `ReasoningDelta` / `thought` | 思考中显示 `· Thinking...`；正文开始或一轮结束时定稿成 `+ Thought: N.Ns`（accent），正文块收起 |
| `ToolStarted` | 建立同组标题、开放主体和折叠行，按调用 id 保存 |
| `ToolOutput` | 追加到对应开放主体，并行调用不会串卡 |
| `ToolFinished` | 用结构化 View 定稿名称、参数、右对齐统计和主体 |
| `Compacted` | 永久的压缩前后 token 系统块 |
| `Notice` | error 留永久块；`persistent=true` 的审批审计留正文；其它 info/warn 进 toast |
| 带 parent_session_id 的事件 | 按 model_call_id 归位：在对应 task 块正文追加子 Agent 的工具行，同时把事件喂给该子会话的 Pane |
| `TurnEnded` | interrupted/denied/limit/failed 留系统块；done 追加 `▣ 模式 · 模型 · 耗时` 尾行 |

工具标题是状态符 + 加粗名称、muted 参数、右对齐统计三段；参数过长时省略。主体每行用 `│ `，折叠行独立用
`└ `，同一工具三个块共享 group 且只有标题有上边距。默认折叠：edit/write 20 行、bash 10 行、grep/glob/MCP 5 行、
非结构化结果 3 行。Read 和 Plan 通常没有主体。`TaskView` 的统计是 `N steps · M tools · S.Ss`，展开后先列逐条
工具摘要（错误项用 ✗），空行后是子 Agent 的最终文本。

思考正文默认收起（`collapsed_rows = 0`），Ctrl+R 统一展开或收起本会话的全部思考，标题前缀随之在 `+` 与 `-` 之间切换。

`TodoView` 到达时，Transcript 把整份列表交给 SidePanel，对话只留下 Plan 工具标题；全部完成的首次更新追加一条
`plan complete` 系统块。宽度少于 80 列时 SidePanel 让位，Transcript 原位替换一个计划块，不为每次更新新增消息；
宽度恢复后该块清空并由侧栏接管。`/new` 清空列表，恢复会话依靠最后一次 TodoView 自然重建。

### 子会话 Pane

Shell 持有 `std::vector<Pane>`：索引 0 恒为主会话，task 的子事件首次到达时按父调用 id 建一个子 Pane
（标题 `task · agent`，记录子会话 id），之后该子会话的事件同时喂给这个 Pane 的 Transcript。Pane 的
Scrollback 由 `ScrollFrame` 统一持有，`show()` disown 旧区、adopt 新区；鼠标处理器每个 Pane 一个，
切换时解绑旧的、绑定新的。Ctrl+A / `/agents` 打开 Panel 列出主会话与全部 task；子 Pane 下输入框禁用，
Ctrl+C 或再次切换回到主会话。

活动期之外的子会话没有 Pane：恢复父会话时只还原 task 块，用户从 `/agents` 选中某个 task 时才用
`session.history` 分页读取子会话记录并投影进新 Pane。浏览是只读查询，不修改子会话记录或 updated。子 Pane 下提交输入被拒并保留草稿。

## 6. 状态、浮层与权限

StatusLine 左段是缩成 `~` 的项目路径，右段右对齐显示上下文用量（`14.1k (12%)`，超过触发百分比转 warning）
与 `ctrl+p commands`；侧栏收起或窄屏时在用量前加计划进度。模型、权限模式在输入框尾行与消息尾行里显示，
会话标题、MCP 与项目分支在 SidePanel 里显示。

SidePanel 自上而下是会话标题（首条输入的首行）、`Context` 与 token 用量、MCP ready/总数、当前计划，
底部两行固定为 `路径:分支` 与版本号；左缘一列是与主区的分隔线。Ctrl+T 收起或展开整个侧栏。

Panel 由标题、可选内存过滤、三列列表和底部提示组成，最大宽 76 列、高度不超过终端 60%。上下移动跳过不可用项，
Enter 执行，Esc 关闭。会话列表和文件候选的 I/O 不发生在渲染线程。

ToastStack 是单一右上角 overlay，内部维护最多三条通知，新通知在上；info/warn/error 分别使用 `·`/`⚠`/`✗`。
错误 Event 已永久进入 Transcript，因此不重复弹 error toast。权限对话框使用 y/a/n/e/Esc/Ctrl+C，
使用 active 圆角边框、`Approval needed` 标题、英文 reason 与 session rule、可滚动预览和横排选项。
长意图和 session rule 放在可折行的预览区，预览与输入不占用边框列。
审批预览同时显示实际 cwd、模式和每项增量权限原因；敏感/受保护请求不显示 session 选项。
底边单独预留一行，避免覆盖选项；紧凑选项为 `[y] allow / [a] session / [n] deny / [e] explain`。
请求条目按 kind 显示（命令 / 读取 / 写入 / 网络目标 / 敏感读取 / 受保护写入 / host access），
host access 附完整宿主边界说明，一次性请求说明不能会话允许，部分执行显示警告。
同一模态骨架也显示 Question：数字或上下键选择，Enter 确认，多选用空格，Other 进入自由输入，Esc 取消。
权限与问题浮层互斥。子 Agent 发起的审批在标题里追加 `· via task · <agent>`，与主 Agent 自己的请求区分。
unrestricted 的输入/消息尾行使用 error 色，plan 使用 accent 色。长计划条目与通知按显示列宽截断。
`/permissions` 复用 Panel 显示当前内存授权，Enter 撤销所选规则；空列表只显示不可选提示。

符合条件的子权限请求由父模型审阅，显示 `Reviewing subagent permission`，不打开人工模态框。
决定以持久 Notice 保留来源、范围与理由；历史 `permission/parent_review` 投影为相同的审计提示。
路由条件、失败和降权行为见 [权限指南](../guide/permissions.md)。

## 7. 退出与错误

`/exit`、连续 Ctrl+C 或进程信号都走同一退出流程：请求当前 Run 取消，Runtime 退出并还原终端，随后前端发 `backend.shutdown`
等待后端收尾（宽限 10 秒，超时回收自己创建的后端），最后在普通终端打印恢复命令。后端连接意外结束时界面显示错误并退出，
不重连、不重发输入。

单轮 failed、denied、limit 或模型连接失败只结束本轮，界面仍可继续接受输入。正常退出返回 0，进程信号退出返回 130，
启动或通信失败返回 1。


## 模型切换

`/model` 或 Ctrl+M 打开通用 Panel，展示配置名、kind、模型 ID，当前项标 current，支持搜索。
Ctrl+M 需要终端提供可区分的扩展按键编码；传统终端把它与 Enter 编成相同字节时使用 `/model`。
命令面板在忙碌时禁用切换；忙碌时直接输入 `/model` 也会提示不可用。

模型 Panel 的 `a add` 打开七步表单：协议、配置名、base URL、模型 ID、API key（也可填 `env:VARIABLE`）、
最大输出和上下文窗口。provider 种类与默认端点来自 `model.list`；Enter/Tab 前进，Shift+Tab 返回，Esc 取消。
提交发 `model.add`（密钥只作为这次请求的 write-only 字段），成功后刷新列表并自动切换；失败用 error toast 显示校验或写入错误，
保存成功但切换失败时如实提示。

选择后发 `session.select_model`，后端在空闲时按 B13/B14 准备候选、复用写租约并安装（见 [runtime](runtime.md#替换会话)）。
成功后前端更新输入框、后续消息尾行和 Context 预算；历史对话不清空，session ID 不变。失败弹 error toast，
后端保留当前会话。历史条目带当时的模型标签。密钥不进入面板、协议 DTO 或日志。

本地 Qwen 与 Ollama 的跨协议切换、失败保留和回放已真实验证；远端切换尚未验证。

## Skill selection

`/skills` opens the skill catalog and discovery diagnostics. Selecting a skill inserts `$name` without submitting; typing `$` opens skill completion. Both reuse the existing application panel/completion widgets and fetch metadata through `skills.list`. Skill tool results use a dedicated card projection. See [skills](skills.md).

历史加载各自拥有条目缓冲和分页游标；切换会话会取消旧加载，完成或失败即释放。主会话实时事件始终进入主文档，子事件按会话身份进入子文档。
