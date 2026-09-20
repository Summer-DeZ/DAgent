# ui：应用层交互界面

`dagent` 不带子命令时进入全屏终端界面。公开头文件在 `src/public/ui/`，实现在 `src/private/ui/`，
库为 `dagent_ui`。界面只消费 agent Event、提交输入并返回权限决定；模型循环、工具执行和权限规则仍属于核心。
它只使用冻结的 [TUI 框架](tui-framework.md)，没有为应用视觉增加框架原语。

界面 chrome 使用英文，统一来自 `ui/strings.hpp` 的 `Strings` 和 `ui/strings.cpp` 的常量表。
`text()` 只返回英文表的常量引用；字段全为 `string_view`，格式串使用显式参数索引，由 `format_text()` 格式化。
暂不提供语言配置、运行时切换或外部文案文件。工具结果、模型提示词、日志和 CLI 不进入此表；
模型正文和用户提供的计划标题仍跟随用户语言，旧会话的历史文本不迁移。

## 1. 组成与控件树

公开入口 `run_interactive(agent::Setup, InteractiveOptions, agent::Interrupts&)` 负责加载主题、创建或恢复 Agent，
然后进入内部 `Shell`。主要部件如下。

| 部件 | 职责 |
| --- | --- |
| `Shell` | 控件寿命、工作队列、Agent 替换、命令、事件分派和退出 |
| `Transcript` | Event 到 Document 块的实时/回放共用投影 |
| `PromptBox`、`PromptInput` | 自动折行、按行数增高的多行输入；左侧竖条与框内尾行；发送、排队和取回 |
| `StatusLine` | 底部一行：项目路径、上下文用量与命令面板提示 |
| `Completion` | `/` 命令和 `@` 文件的贴输入框补全浮层 |
| `Panel` | 命令、会话、主题和帮助共用的居中列表浮层 |
| `ToastStack` | 右上角最多三条、五秒到期的瞬时通知 |
| `SidePanel` | 右侧常驻信息栏：会话标题、上下文用量、MCP、最近一份 `TodoView`、项目与版本 |
| `ApprovalDialog` | 权限原因、意图预览、反馈输入和跨线程回答 |

```text
LayerStack
└─ Container(vertical)
   ├─ Container(horizontal)             主区 + 计划栏
   │  ├─ Container(vertical)            主区
   │  │  ├─ Center(Scrollback)          flex 1，内容栏最大 88 列
   │  │  ├─ Center(Activity)            content，空闲高度 0
   │  │  ├─ Center(QueueLabel)          content，最多 3 行
   │  │  └─ Center(PromptBox)           content，1–8 行正文 + 1 行尾行
   │  └─ SidePanel                      content，26/30 列或宽 0
   └─ StatusLine                        fixed 1，铺满终端
浮层：Completion、Panel、ApprovalDialog、ToastStack
```

`centered()` 在主区至少留四列边距，内容栏最大 88 列；主区窄于 44 列时取消边距。
对话、活动、排队和输入使用同一宽度与左缘。侧栏被收起或终端少于 80 列时宽度为 0；
80–99 列占 26 列，至少 100 列占 30 列。状态栏始终在计划栏之外铺满。

新会话的第一个 Document 块是欢迎头，包含版本、项目、模型和 MCP 数量；内容栏少于 60 列时只画单行字标。
恢复会话改画恢复 id 与回放事件数。Scrollback 不贴底时，右下角覆盖显示未读行数，End 回到底部。

## 2. 主题

`ui.theme_file` 指向 JSON 主题；文件在内置 dark / light 令牌上覆盖同名项，支持顶层 `name`、`defs`、
`dark` 和 `light`。`list_themes()` 在工作线程枚举并预加载同目录的 `*.json`，解析失败的条目保留但不可选择。

Shell 的所有主题变更都经过 `apply_theme()`：复制令牌、递增 `epoch`，再依次刷新 Scrollback、Activity、
排队行、PromptBox、HintLine、StatusLine、TodoPanel、ToastStack、Panel、ApprovalDialog 和 Completion。
`/theme` 面板提供内置 dark、内置 light、跟随终端背景和主题文件的 dark / light 两个版本；打开时选中当前主题，
不触发预览。移动选择使用已加载令牌即时预览，Enter 保留，Esc / Ctrl+C 恢复打开前的主题。
内置明暗主题显式设置底色和正文前景；普通文字令牌继承主题底色，状态栏、面板、toast 和计划栏继承各自
panel 背景，不在底纹上打孔。切换时使整个控件树失效，两侧留白也使用新底色。

视觉角色只使用 `ThemeTokens`：正文 `text`，次要信息 `text_muted`，用户竖条与选中项 `primary`，
进行中与自动编辑 `accent`，工具完成/失败分别用 `success`/`error`，上下文阈值和重试用 `warning`。
用户消息和选中行用 `background_element`；计划栏、状态栏和浮层用 `background_panel`。

## 3. 线程与生命周期

控件、Document、浮层和事件处理器只由渲染线程访问。Shell 有一个串行工作线程；以下操作进入其阻塞队列：

- `Agent::run_turn`、手动压缩、新建和恢复会话；
- `session::list`、git 环境收集和文件补全扫描；
- Agent 产生的 Event 按值捕获后用 `Runtime::post` 回到渲染线程。

审批由工作线程等待一次性 future，渲染线程打开模态对话框；回答与 stop callback 争用同一原子完成标志。
`/new` 和会话恢复都在工作线程构建新 Agent，锁内交换指针、锁外析构旧对象。恢复成功后清空当前投影并用
同一个 `Transcript::apply` 回放；失败只弹 error toast，保留当前会话。

Shell 进入全屏后调用 `Terminal::set_mouse(true)` 打开鼠标上报，ScrollbackMouse 才收得到滚轮、
拖选与双击；退出、挂起和 `run_external` 的还原由框架处理。

活动动画忙时每 100 ms 更新；MCP 只在连接中或轮次忙碌时每 200 ms 取状态，稳定空闲后停止。
toast 使用一次性五秒定时器，文件补全使用一次性 80 ms 防抖；空闲时没有新增的周期唤醒。

## 4. 输入、补全与排队

PromptBox 左侧是一根竖条（忙碌时换成 `primary`），底纹用 `background_element`，正文上方留一行、
与框内最后一行之间再留一行，输入时不贴着上面的对话；最后一行显示 `权限模式 · 模型`。正文按内容栏宽度折行，控件高度跟着折行结果在 1–8 行之间变化（加上三行留白与尾行，整体 4–11 行），超过 8 行在框内滚动并保证
光标可见；空输入显示占位文字，占位文字不属于 `text()`。

框架的 `tui::InputBox` 只做横向滚动、不折行，所以 PromptBox 自绘正文，不调用基类 `render`；
基类内部的横竖滚动量因此恒为 0，`cursor()` 借这一点从基类光标反推逻辑行号与显示列（基类没有公开它们）。

补全列表的名称和描述使用统一列起点，按字素的终端显示宽度计算列宽；中文、宽字符和窄屏截断不以 UTF-8
字节数计宽，长内容用省略号收尾，避免侵入右侧边框。

输入和斜杠命令共用 FIFO 队列。忙时继续 Enter 会排队；QueueLabel 第一行显示数量，随后显示最近两条的首行。
输入框为空时 ↑ 取回最后一条排队输入。收到 `TurnEnded` 或压缩完成后自动处理下一条。

| 按键 | 行为 |
| --- | --- |
| Enter | 发送；忙时排队；浮层打开时确认选中项 |
| Shift/Alt+Enter | 输入换行 |
| Esc | Interrupt turn（先关闭补全/面板；否则中断当前轮） |
| Ctrl+C | Clear or exit（先关浮层；忙时中断；有输入时清空；空闲连续两次退出） |
| Shift+Tab | Cycle permission mode（ask / auto-edit 切换） |
| Ctrl+O | Expand tool output（已完成工具输出展开/折叠） |
| Ctrl+R | Expand thoughts（全部思考展开/折叠） |
| Ctrl+T | 收起或展开右侧信息栏 |
| Ctrl+T | Toggle plan panel（展开/收起；无计划时弹 toast） |
| Ctrl+P | Command palette（命令面板） |
| Ctrl+? | Keys and commands（帮助面板） |
| PgUp/PgDn、Home/End | Page up/down、Go to top/bottom（翻页、回顶、回底） |

输入是单行且以 `/` 开头时，Completion 从 Shell 注册的命令表按前缀过滤。Tab 只补全，Enter 直接执行，
不把命令提交给模型。命令表同时驱动 ctrl+p 与帮助面板，避免维护第二份标题和分类。

光标前最后一个非空白串以 `@` 开头时打开文件补全。工作线程用 `workspace::files` 建一次项目文件缓存，
再用 `fuzzy_rank` 取前八项；每次请求带 Completion 代次，过期结果被丢弃。Enter 用仓库相对路径替换 `@式` 并补空格。

| 命令 | 行为 |
| --- | --- |
| `/new` | New session（清空对话和计划） |
| `/compact` | Compact context（手动压缩，可取消） |
| `/sessions` | Switch session（异步列出并恢复本项目会话） |
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
| `TurnStarted` / `user` | `▌` 竖条、正文和整行 `background_element` |
| `TextDelta` | `MarkdownStream`，正文前留两列 |
| `ReasoningDelta` / `thought` | 思考中显示 `· Thinking...`；正文开始或一轮结束时定稿成 `+ Thought: N.Ns`（accent），正文块收起 |
| `ToolStarted` | 建立同组标题、开放主体和折叠行，按调用 id 保存 |
| `ToolOutput` | 追加到对应开放主体，并行调用不会串卡 |
| `ToolFinished` | 用结构化 View 定稿名称、参数、右对齐统计和主体 |
| `Compacted` | 永久的压缩前后 token 系统块 |
| `Notice(error)` | 永久 error 块；info/warn 只进 toast |
| `TurnEnded` | interrupted/denied/limit/failed 留系统块；done 追加 `▣ 模式 · 模型 · 耗时` 尾行 |

工具标题是状态符 + 加粗名称、muted 参数、右对齐统计三段；参数过长时省略。主体每行用 `│ `，折叠行独立用
`└ `，同一工具三个块共享 group 且只有标题有上边距。默认折叠：edit/write 20 行、bash 10 行、grep/glob/MCP 5 行、
非结构化结果 3 行。Read 和 Plan 通常没有主体。

思考正文默认收起（`collapsed_rows = 0`），Ctrl+R 统一展开或收起本会话的全部思考，标题前缀随之在 `+` 与 `-` 之间切换。

`TodoView` 到达时，Transcript 把整份列表交给 SidePanel，对话只留下 Plan 工具标题；全部完成的首次更新追加一条
`plan complete` 系统块。宽度少于 80 列时 SidePanel 让位，Transcript 原位替换一个计划块，不为每次更新新增消息；
宽度恢复后该块清空并由侧栏接管。`/new` 清空列表，恢复会话依靠最后一次 TodoView 自然重建。

## 6. 状态、浮层与权限

StatusLine 左段是缩成 `~` 的项目路径，右段右对齐显示上下文用量（`14.1k (12%)`，超过触发百分比转 warning）
与 `ctrl+p commands`；侧栏收起或窄屏时在用量前加计划进度。模型、权限模式在输入框尾行与消息尾行里显示，
会话标题、MCP 与项目分支在 SidePanel 里显示。

SidePanel 自上而下是会话标题（首条输入的首行）、`Context` 与 token 用量、MCP ready/总数、当前计划，
底部两行固定为 `路径:分支` 与版本号；左缘一列是与主区的分隔线。Ctrl+T 收起或展开整个侧栏。

Panel 由标题、可选内存过滤、三列列表和底部提示组成，最大宽 76 列、高度不超过终端 60%。上下移动跳过不可用项，
Enter 执行，Esc 关闭。会话列表和文件候选的 I/O 不发生在渲染线程。

ToastStack 是单一右上角 overlay，内部维护最多三条通知，新通知在上；info/warn/error 分别使用 `·`/`⚠`/`✗`。
错误 Event 已永久进入 Transcript，因此不重复弹 error toast。权限对话框保留 y/a/w/n/e/Esc/Ctrl+C 语义，
使用 active 圆角边框、`Approval needed` 标题、英文 reason 与 session rule、可滚动预览和横排选项。
长意图和 session rule 放在可折行的预览区，预览与输入不占用边框列。
底边单独预留一行，避免覆盖选项；紧凑选项为 `[y] allow / [a] session / [w] network / [n] deny / [e] explain`。
长计划条目与通知按显示列宽截断，ASCII `...` 不覆盖边框。

## 7. 退出与错误

退出会请求当前轮停止、关闭工作队列并让 Runtime 退出。Runtime 返回后先还原终端，再 join 工作线程并析构 Agent，
最后在普通终端打印恢复命令。全屏期间进程信号走同一优雅路径；第二次外部信号仍可由进程级机制强制结束。

工作线程最外层捕获未预期异常，记录日志、向对话追加错误并退出。单轮 failed、denied、limit 或模型连接失败只结束
本轮，界面仍可继续接受输入。正常退出返回 0，进程信号退出返回 130，工作线程异常返回 1。


## 模型切换

`/model` 或 Ctrl+M 打开通用 Panel，展示配置名、kind、模型 ID，当前项标 current，支持搜索。
Ctrl+M 需要终端提供可区分的扩展按键编码；传统终端把它与 Enter 编成相同字节时使用 `/model`。
命令面板在忙碌时禁用切换；忙碌时直接输入 `/model` 也会提示不可用。

工作线程重新解析所选配置与密钥，通过 Agent::resume 恢复同一会话，锁内替换 Agent 指针、锁外析构旧对象。
切换期间显示 switching model，普通输入排队；成功后在渲染线程更新输入框、后续消息尾行和 Context 预算。
历史对话不清空，session ID 不变。失败弹 error toast，保持当前 Agent，随后继续处理排队输入。
ModelChanged 事件使恢复后的历史消息仍显示当时的模型标签。密钥不进入面板或日志。

本地 Qwen 与 Ollama 的跨协议切换、失败保留和回放已真实验证；远端切换仍等待可用密钥，
详见 [验收记录](../next-to-do/validation.md)。
