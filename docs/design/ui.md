# ui：应用层交互界面

`dagent` 不带子命令时进入全屏终端界面。公开头文件在 `src/public/ui/`，实现在 `src/private/ui/`，
库为 `dagent_ui`，只依赖 client、protocol、tui 与 base。界面运行在前端进程，只持有 `client::Client`、协议 DTO
与页面状态：业务操作都经 [私有协议](protocol.md) 发给自己的后端，执行状态、队列、模式与模型的真值都在后端 runtime。
它使用 [TUI 框架](tui-framework.md)；phase 4 解冻框架，新增通用超链接样式和块可见性查询；phase 5a 增加颜色令牌与显式 256 色回退，
phase 5b 的对话结果挂钩、卡片、圆角输入框与未读提示也全部在应用层完成，没有新增框架接口。
双栏 diff、文件解析与阅读栏宽度策略仍放在应用层；导航栏、自适应正文宽度与 Layout 设置属于 phase 5c。

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
| `PromptBox`、`PromptInput` | 自动折行、按行数增高的多行输入；圆角边框、提示符与框内尾行；发送、排队和取回 |
| `HeaderBar` | 顶部品牌、会话标题、权限/模型 Chip 与 git 分支；侧栏隐藏时显示模型与上下文百分比 |
| `AgentTabs` | HeaderBar 下方的主会话与子 Agent 标签、状态、选中下划线与鼠标切换；无子 Pane 时隐藏 |
| `Activity`、`QueueLabel` | 当前轮耗时、输出 token 与生成速率；单行队列 Chip 和取回提示 |
| `HintBar` | 底部项目路径与随浮层、审批、运行状态变化的键位提示 |
| `Chip` / `appearance` | 标签绘制、外观令牌变换、统一 Unicode/Nerd/ASCII 图标表 |
| `SettingsDialog` | 外观选项、即时预览、后端保存与撤回 |
| `Completion` | `/` 命令、`@` 文件和 `$` 技能的贴输入框补全浮层 |
| `Panel` | 支持分组、匹配高亮、别名与快捷键列的居中列表浮层；可选详情栏、状态行、来源页脚与行颜色 |
| `ModelPicker` | 两栏模型选择、OpenCode Go 设备登录和提供方模型清单；只经 RPC 操作 |
| `WelcomeView` | 新会话字标、状态 Chip、可恢复的最近会话与快捷键提示 |
| `ToastStack` | 右上角最多三条、五秒到期的瞬时通知 |
| `SidePanel` | 可左右放置或隐藏的 SESSION / CONTEXT / PLAN / AGENTS / CHANGED / MCP 分节栏与底部版本 |
| `ApprovalDialog` | 权限审批与选项提问共用的模态骨架与输入；回答经 `interaction.answer` 提交 |
| `projection` | 协议 DTO / View JSON → UI 实际消费的字段；子事件由 Shell 按协议信封路由，解码后直接投影，不重复包装身份 |

```text
LayerStack
└─ Container(vertical)
   ├─ HeaderBar                         content，filled 1 行；outline 加 1 行底部分隔线
   ├─ AgentTabs                         content，有子 Pane 时 2 行（标签 + primary 下划线）
   ├─ Workspace(horizontal)             主区 + SidePanel（可左右交换）
   │  ├─ Container(vertical)            主区
   │  │  ├─ ScrollFrame                 flex 1，主区全宽；WelcomeView / Scrollback；有未读时底部独占提示行
   │  │  ├─ Center(Activity)            content，空闲高度 0
   │  │  ├─ Center(QueueLabel)          content，有排队输入时 1 行，最多 3 个 Chip + 余数
   │  │  └─ Center(PromptBox)           content，1–8 行正文；始终圆角边框，密度决定留白
   │  └─ SidePanel                      content，26/30 列或宽 0
   └─ HintBar                           fixed 1，铺满终端
浮层：Completion、Panel、ModelPicker（含 Sign-in）、ModelDialog、SettingsDialog、ApprovalDialog、ToastStack
```

`centered()` 在主区至少留四列边距，内容栏最大 88 列；主区窄于 44 列时取消边距。
普通对话块、欢迎页、活动、排队和输入使用同一宽度与左缘。ScrollFrame 占满主区，
TranscriptRenderer 在每种块的测量与物化阶段保留该阅读宽度，并用无源空白 span 居中；
只有满足阈值的双栏 diff 主体可超出阅读栏：主区两侧各至少留 2 列，主体总宽度最多 176 列，与 88 列阅读栏同轴居中。
工具标题、统一 diff 与折叠 footer 仍为原宽度。侧栏被收起或终端少于 80 列时宽度为 0；
80–99 列占 26 列，至少 100 列占 30 列。HeaderBar、AgentTabs 与 HintBar 始终铺满终端；Sidebar 左右交换不影响主区的居中规则。

新建且尚未发送消息的会话由 ScrollFrame 显示独立 WelcomeView，不向 Transcript 写欢迎 Banner；
启动新会话和 `/new` 均显示；初始化的 system 历史记录不算用户消息，不影响欢迎页可见性。
首次提交普通消息或收到 turn_started 时隐藏，恢复会话时也不显示。内容栏少于 60 列或 ASCII 图标集时使用单行字标。
历史 `turn_end` 保留在协议与存储中；历史 DTO 没有轮次耗时，回放不伪造实时尾行。
文本显示宽度和带省略号的裁剪统一使用 `ui/display`；工具折叠提示在极窄宽度仍按列硬裁剪。
恢复会话改画恢复 id 与回放事件数。Scrollback 不贴底且有未读行时，ScrollFrame 在其底部独占一行显示
`N new lines · End to jump`，从实际滚动视口扣除这一行，不覆盖内容；没有未读行时不占空间。
该行位于活动行、队列与输入框上方，End 回到底部后消失，切换 Pane 时取当前 Pane 的未读数。

## 2. 外观与设置

`ui.appearance` 的十一个可选字段为 theme、mode、background、accent、panels、density、icons、sidebar、diff、links，
默认值与校验见 [app 配置](app.md)。当 theme 缺省时继续使用 `ui.theme_file`；两者都没有时使用 dagent。
只保留 dagent，从 Home 的 `themes/dagent.json` 加载；自定义路径相对 Home。
midnight、paper 名称报配置错误，提示改用 dagent 或主题文件路径。
文件格式保留 name、defs、dark、light，缺失令牌由应用层 `builtin_theme()` 补足。

外观初始化链路为 config → ConfigurationGateway → `app.initialize.ui.appearance` → Shell；
后端返回补齐默认值且 theme 为绝对路径的对象，并提供 `ui.themes_directory`。
前端在首帧前应用；后续所有令牌变更经过 `apply_theme()`，提升 epoch 并刷新控件树与全部 Pane。
mode=auto 随 OSC 11 背景通知切换；dark/light 固定。自定义背景的相对亮度优先决定主题明暗变体。

背景令牌为 background（页面）、background_panel（栏与对话框）、background_element（用户消息与 Chip）、
background_selected（Panel、补全、模型/Agent 选择器、设置和欢迎页会话的选中行）。
solid 保留主题底色；transparent 只将页面背景设为终端 default，让终端自身的壁纸、透明度或模糊透出。
`#rrggbb` 背景按现有相对亮度规则分为暗色（≤0.5）和亮色：暗色向白混合 5%/10%/15% 得到面板/元素/选中，
亮色向黑混合 3%/6%/9%；文字与边框使用相应明暗主题。

文字三档：text 用于正文与列表内容；text_muted 用于工具参数、统计、时间戳、分节标题、提示说明、Chip 的次要标签；
text_faint 用于行号、hunk 行、普通轮次尾行、diff gutter、注释、侧栏版本、提示分隔符、`⎿` 结果挂钩、`…` 折叠行和输入占位文字。
text_faint 只落在页面或面板底色上；元素或选中底色上的次要文字必须用 text_muted。
代码块使用页面底色，注释为 text_faint 加斜体。markdown_heading 为正文色加粗，error 不加粗，
markdown_link 为 info 色加下划线。diff_context 为正文色，diff_hunk 为 text_faint。

accent 配置接受 orange（缺省值）、green、violet、blue 或 `#rrggbb`，预设按名字保存：

| 名称 | dark | light |
| --- | --- | --- |
| orange | `#fe8a4c` | `#a85500` |
| green | `#87d787` | `#2a7a1a` |
| violet | `#b9a0ff` | `#6a4bc4` |
| blue | `#7a9cff` | `#3157c8` |

旧预设 hex（不区分大小写）`#EC5B2B` / `#7fd17f` / `#c792ea` / `#6ba1e6` 加载时映射为对应名称；
其它 hex 保留为自定义色。自定义色以 OKLab 固定色相调整明度，浅色压暗、深色提亮，
用最终量化到 8 位通道的 RGB 检查在 background_panel 上达到 4.5:1；超出 sRGB 色域时降低色度。
配置仍保存用户原始 hex。primary 使用最终强调色，accent 令牌为 primary 向 text 混合 20%，
selection 为页面色文字与 primary 底色（透明页面使用该明暗主题的 solid 页面色作文字）。
真彩直接输出 RGB；256 色优先使用颜色的显式索引，否则按框架规则量化。

filled 使用主题填色，HeaderBar 与 HintBar 均为面板底色；PromptBox 在两种模式下始终使用页面底色与 border_active 圆角边框。
outline 的 HeaderBar、HintBar、SidePanel、Chip 使用页面底色，
分别画底部分隔线、内侧竖线，Chip 只显示标签且相邻间隔两列。浮层始终使用面板填色。
HeaderBar 的正文仍为一行，outline 另占一行画底线，避免把线画在标题文字上。
compact 取消 Transcript 用户消息前的轮间空行和侧栏分节空行；PromptBox 在两种密度下均不加上下 padding，保留边框行。
Ctrl+T 只改变本次会话的侧栏显示状态，不写配置；设置 Esc 撤回也恢复打开前的临时显示状态。

`/settings` 和命令面板的 Settings 打开居中圆角浮层（最大 100×24）。左侧只有 Appearance，
中间九行选项（Mode 起首，包含 Diff 与 Links，无 Theme 行），右侧是 token 预览；终端少于 80 列时不画预览，短终端滚动选项。
Diff 行循环 auto / unified，Links 行循环 on / off，均即时预览并走同一保存/撤回路径。
选中 Diff 时，预览区显示两行双栏样例（增删行与上下文），不受样例窄宽度的实际 diff 阈值限制。
上下选择，左右循环；背景支持 solid / transparent / custom，强调色向右依次为
orange → green → violet → blue → custom，向左反向；预设显示名称，custom 显示十六进制值。
选中 custom 后 Enter 编辑十六进制字段；不合法的输入以 error 显示且不应用。
有效颜色 Enter 应用后，再次 Enter 保存；也可移到普通选项行 Enter 保存。
所有有效更改立即预览；`/theme` 是打开 Settings 并定位 Mode 行的别名。
Enter 保存经 `ui.appearance.save`，成功关闭并提示 Appearance saved；失败保留对话框并弹错误 toast。
Esc / Ctrl+C 还原打开前外观，不写文件。保存请求已发出时锁定编辑直到收到结果。

应用图标集中在 `ui/appearance` 的一张表，所有动画调用者使用同一组四帧及其长度。
Unicode 为默认；Nerd 在以下对话图标上与 Unicode 相同，品牌、分支、勾、叉、文件夹继续使用专用字体码位。
ASCII 的结果挂钩多占一列，后续结果与折叠行按实际图标宽度对齐。

| 角色 | Unicode / Nerd | ASCII |
| --- | --- | --- |
| item | `●` | `*` |
| running | `◐ ◓ ◑ ◒` | `\| / - \` |
| result | `⎿` | `` `- `` |
| thought | `✻` | `~` |
| user / prompt | `›` | `>` |
| todo | `☐` | `[ ]` |
| done | `☒` | `[x]` |

旧方框消息图标已移除。活动行、工具调用、子任务、AgentTabs、侧栏 Agent 状态和登录等待使用统一 spinner。
正文与工具输出本身不做字体转换。

dagent 使用计划 §9.3 的中性灰阶与橙色，深浅两套值及文件格式见 [DAgent 配色](opencode-theme.md)。
层次通过亮度、间距、字重和 Chip 表达；phase 5a 实现颜色，phase 5b 实现下述对话结构。

## 3. 线程与生命周期

控件、Document、浮层和事件处理器只由渲染线程访问。前端没有业务工作线程：

- 输入、命令、权限切换、回答等发 RPC；同步等待只用于启动阶段，运行中都用 `call_async`，结果 `post` 回渲染线程。
- 后端事件由 Client 读线程经 `FrontendBridge` post 到渲染线程；`CallbackGate` 保证 Shell 析构后在途回调不再触碰页面。
- Shell 记录当前 session_id 与 generation，丢弃旧 generation 的事件；`session.changed` 后取 `session.snapshot` 刷新标签。
  new/resume 时清空投影并用 `session.history` 分页回放；切模型保留 Transcript，只更新标签与上下文。
- 列表、历史、项目信息与文件补全都是后端查询；主会话与子 Pane 各有一个历史分页器，关闭页面时发 `session.history_close`。

Shell 进入全屏后调用 `Terminal::set_mouse(true)` 打开鼠标上报，ScrollbackMouse 才收得到滚轮、
拖选与双击；退出时的终端还原由框架处理。

活动动画忙时每 100 ms 更新，同时更新运行中的工具、任务标签和 AGENTS 耗时；MCP 状态来自会话快照：只在连接中或轮次忙碌时每 200 ms 请求一次 `session.snapshot`，稳定空闲后停止。
toast 使用一次性五秒定时器，文件补全使用一次性 80 ms 防抖；设备登录等待期间每 100 ms 更新 spinner 与到期倒计时，离开或出错即停止。空闲时没有新增的周期唤醒。
浏览器由前端用 argv 调用 `xdg-open`（Linux）/ `open`（macOS），独立进程会话/进程组，标准流重定向到 `/dev/null`；只用一个 detached 回收线程等待子进程退出。

## 4. 输入、补全与排队

PromptBox 在 filled / outline 两种面板模式下都画 `border_active` 圆角线框，内部保留页面背景，
首行以 accent 的 `› ` 引出输入，续行与输入正文对齐，占位文字使用 text_faint。
尾行左侧复用 Chip helper 绘制权限模式、模型：模式与 HeaderBar 同色（普通 muted、plan 为 accent、
unrestricted 为 error），模型为 text。右侧 muted 显示 `↵ send · ⇧↵ newline`，忙时为
`↵ queue · ⇧↵ newline`；ASCII 图标集改用 `enter` / `shift+enter`。窄宽度先裁剪模型，
不足以同时容纳标签和完整快捷键时省略快捷键，不重叠绘制。
正文按框内可用宽度折行，正文高度为 1–8 行，超出后滚动并保持光标可见。
comfortable 和 compact 均只在框内绘制输入行与标签/提示尾行，不加上下 padding；
单行输入时为上框线、输入、尾行、下框线共四行，多行输入按实际正文行数增高。
filled 的 Chip 保留元素底色，outline 继续使用无填色 Chip。

欢迎页的状态 Chip 包含模型；模型同时保留在 HeaderBar、输入框尾行和消息尾行。

框架的 `tui::InputBox` 只做横向滚动、不折行，所以 PromptBox 自绘正文，不调用基类 `render`；
基类内部的横竖滚动量因此恒为 0，`cursor()` 借这一点从基类光标反推逻辑行号与显示列（基类没有公开它们）。

补全列表的名称和描述使用统一列起点，按字素的终端显示宽度计算列宽；中文、宽字符和窄屏截断不以 UTF-8
字节数计宽，长内容用省略号收尾，避免侵入右侧边框。

普通输入经 `input.submit` 进入后端内存 FIFO，忙时也立即提交。QueueLabel 仅在队列非空时占一行：
muted 的 `QUEUED`、按 FIFO 顺序最多三个 Chip、其余 `+N`，右侧 muted 的 `↑ recall`。
每个 Chip 使用输入首行，最多 24 个终端显示列（包括省略号），窄屏按可用空间进一步裁剪；
沿用 filled / outline Chip 样式和统一图标表。输入框为空时 ↑ 发 `input.recall_last`，
把最后一条仍排队的输入取回输入框；重新提交会重新记录提交时间。
后端在 `turn_ended` 或压缩完成后自行处理下一条。

Activity 位于对话和队列之间，忙时一行，空闲零高度。左侧为 accent spinner 与
`Working · 23.4s · 1.8k tokens · 64 tok/s`，右侧 muted 的 `esc interrupt`。
token 数为零时只显示阶段与耗时（如 `Working · 0.1s`）；有输出量且有流式用时后才显示速率。
宽度不足时依次整项省略速率、token 数、`esc interrupt`，不截断指标。
耗时从本轮开始累计，跨多次模型调用和工具执行不重置；重试、压缩及等待审批沿用相应阶段名称。
问答对话框打开时显示 `waiting for answer`，审批仍显示 `waiting for approval`。
输出量累计本轮各次模型调用的 `context.completion`，当前调用未报告 usage 时由已流出的正文与思考
按约四个 Unicode 码点一个 token 估计，任何估计部分用 `~` 标记；收到真实 usage 即替换该调用估计。
速率分母是各调用从首次流式输出（含工具调用开始）到完成的时间之和，工具执行、等待首 token、
重试等待不计入。没有流式用时则省略速率。失败重试丢弃当前部分输出的估计与计时，
最终尾行以 `turn_ended.usage.completion` 为准。
现有协议把未报告 usage 编成全零，因此全零无法与“真实零用量”区分：只有 prompt 或 completion
非零才认定为真实报告，不将默认零伪装成实测用量。

斜杠命令不进队列，立即分派：业务命令（new、sessions、model、compact、plan、permissions）按原 busy 条件发请求，
本地视图命令（settings、theme、help、agents、exit、折叠与侧栏）在前端执行。

| 按键 | 行为 |
| --- | --- |
| Enter | 发送；忙时排队；浮层打开时确认选中项 |
| Shift/Alt+Enter | 输入换行 |
| Esc | Interrupt turn（先关闭补全/面板；否则中断当前轮） |
| Ctrl+C | Clear or exit（先关浮层；忙时中断；有输入时清空；空闲连续两次退出） |
| Shift+Tab | 按 ask → workspace → unrestricted → ask 循环权限模式；unrestricted 用 error 色警示 |
| Ctrl+G | 进入或退出 plan 模式 |
| Ctrl+O | Expand tool output（已完成工具输出展开/折叠） |
| Ctrl+D | Toggle side-by-side diff（auto 且有宽度足够的 diff 卡片时，本次会话切换双栏/统一） |
| Ctrl+R | Expand thoughts（全部思考展开/折叠） |
| Ctrl+T | 收起或展开信息栏 |
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
| `/settings` | Settings（实时预览外观，Enter 保存，Esc 撤回） |
| `/theme` | 打开 Settings 并定位 Mode 行 |
| `/help` | Keys and commands（打开只读帮助） |
| `/exit` | Exit（退出） |

## 5. 对话投影

每个顶层对话项左侧有两列 gutter。Assistant 正文首行是 text 色 `● `，后续 Markdown、代码、表格与续行缩进两列。
装饰符、底纹补空格、工具右侧统计使用 `k_no_src`；可见正文 span 保留源偏移，继续支持拖选与链接。
结果首行以两列缩进后的 text_faint `⎿ ` 挂在调用下方，正文起于第 4 列；后续结果行缩进 4 列。
ASCII 挂钩为 `` `- ``，相应缩进为 5 列。计数、折行和物化绘制使用同一可用宽度。

| meta / Event | 显示 |
| --- | --- |
| 恢复提示 | `●` 系统块，恢复 id 与回放事件数；新会话欢迎页不进入 Document |
| `turn_started` / 历史 user | muted `› `、正文和整行 `background_element`；首行右侧 muted 本地 `HH:MM`，历史无时间则留空 |
| `TextDelta` | `MarkdownStream`，本段首行 text `●`，续行缩进两列 |
| `ReasoningDelta` / `thought` | `✻ Thinking…`，符号 accent；结束为 `✻ Thought for N.Ns`，muted 斜体；无正文不显示 |
| `ToolStarted` | 建立同组标题、开放主体和折叠行；accent 四帧 spinner、粗体名称、两空格、muted 参数 |
| `ToolOutput` | 追加到对应主体，`⎿` 首行及缩进续行；并行调用不会串卡 |
| `ToolFinished` | success / error 的 `●`；结构化 View 定稿名称、参数、右对齐 muted 统计，edit/write 的 +N −M 分别为 success / error 色且均不加粗 |
| `TodoView` | `● Plan  done / total`，条目挂在标题下方；侧栏仍同步最新列表 |
| 问答请求 / `AskView` | 运行中 spinner / 完成后 `●` 加 `Ask  <header>`，问题在 `⎿` 后只出现一次；完成后下一行起绘制选项 Chip |
| `Compacted` | 永久的压缩前后 token 系统块，使用 `●` gutter |
| `Notice` | error 留永久块；persistent 审批审计保留内容；匹配级别色的 `●`，多行详情在 `⎿` 下；其它 info/warn 进 toast |
| 带 parent_session_id 的事件 | 按 model_call_id 归位：先更新子会话 Pane，再复用其工具标题与统计原位更新父 task 的嵌套工具行 |
| `TurnEnded` | done 追加两列缩进的 faint `模式 · 模型 · 耗时 · 2.1k out`，无 usage 时省略 out；interrupted 为 error `✗ interrupted · 耗时`；denied/limit/failed 保留系统提示 |

实时主会话消息使用前端 `input.submit` 时记录的本地提交时间，排队期间不改为执行时间；
子会话输入使用收到其 `turn_started` 的时间。时间在用户块 meta 中，不插入可复制的原文。
当前 HistoryItem 没有用户事件时间字段，历史用户消息省略时间，不用恢复时刻替代。
用户块为时间预留右侧列，长正文折行后仍整行填充元素底色，续行不重复提示符。
comfortable 仅在用户消息前留一空行，首条用户消息除外；compact 取消这一空行。
一轮内思考、正文、工具调用、结果和尾行紧接，不添加块间距；正文自身的段落与空行仍保留。
普通尾行不再带图标或模式色；中断只追加带耗时的错误尾行，不向正在生成的回答追加中断说明。

工具标题是状态符 + 加粗名称、两空格、muted 参数、右对齐统计；参数过长时省略，窄屏统计也按剩余列裁剪。
工具主体使用 text_muted，失败与中断输出使用 error；bash、文件列表、grep 和 web 结果均不再画竖线 gutter。
折叠行与结果文字对齐，使用 text_faint：`… N more lines · ctrl+o expand` / `… ctrl+o collapse`；
剩余行数按当前宽度的实际显示行数计算。没有超过折叠阈值的卡片不显示折叠行，也不留空白。
同一工具三个块共享 group。默认折叠：edit/write 20 行、bash/task 10 行、web search 8 行、grep/glob/MCP 5 行、
非结构化结果 3 行；成功 web fetch 正文默认全收起，仅显示 `⎿ N lines · ctrl+o expand`。
Read 通常没有主体。结果首行仅重复标题参数（含 `URL: <参数>` 这样的标签）时删除这一行，其余内容保留。

edit/write diff 隐藏 `---` / `+++` 文件头；hunk 标题独占一行，以 faint 的 `⋯` 加第二个 `@@` 后的函数上下文显示，
无上下文则只显示 `⋯`，不加底色。唯一 hunk 从第 1 行开始时省略标题（新建/清空文件允许另一侧从 0 开始）。
统一 diff 首行使用相同 `⎿` 挂钩，后续行与结果文字对齐：`<num> <sign> <code>`。
新增与上下文使用新文件行号，删除使用旧文件行号，faint 数字列右对齐，列宽取整张卡片最大行号的位数；
折行续行保留列宽但不重复行号与符号。新增、删除行从符号列到行末空白铺满 diff_added_bg / diff_removed_bg 底色，
行号与 gutter 保留页面底色。双栏 diff 仍可突破正文宽度，主体从同一结果行开始，也使用挂钩与续行缩进。
solid 使用主题值（含显式 256 色回退）；自定义背景仍将 success / error 以 15% 混入当前页面底色，
transparent 使用 background_panel 作混色基底。这些动态颜色清除固定索引，按 RGB 量化。

`ui.appearance.diff` 默认 `auto`：先取主区宽度减 4 列与 176 列的较小值，再扣除结果缩进、各侧右对齐行号、空格、符号列和中央分隔符后，
两侧各至少 60 个代码列才使用双栏；`unified` 或空间不足时继续使用上述统一视图。
双栏左为旧文件、右为新文件，中间 faint `│`；上下文在两边显示各自行号，
连续删除与紧随其后的连续新增按行配对，较短侧以无底色的空白补齐。
删除侧使用 `−` 和 diff_removed_bg，新增侧使用 `+` 和 diff_added_bg，底色仅覆盖自身一侧。
双栏与统一视图的删除符号均取图标表的 minus：Unicode / Nerd 为 `−`，ASCII 为 `-`。
长行按字素列宽裁剪并以 `…` 收尾，不折行。hunk 行横跨两侧，以 muted `⋯` 加函数上下文显示，
省略首 hunk 的规则与统一视图相同。折叠阈值与 footer 剩余数使用双栏配对后的显示行数。
Ctrl+D 在本次前端会话切换，主/子 Pane 共用临时状态，不改保存的 diff 值；窄宽度或 unified 配置下无效。
命令面板 Transcript 分组登记 `Toggle side-by-side diff`。空闲 HintBar 仅在当前视口有可切换 diff 主体时
增加 `ctrl+d unified` / `ctrl+d side by side`，滚动、切 Pane、侧栏移动和终端缩放后随可见性更新。

`ui.appearance.links` 默认 `on`，`off` 禁用对话中的 OSC 8 链接。assistant 与工具文本中的裸 http(s) URL、
Markdown 链接文本可点击；Markdown 表格也保留源偏移，因此同样支持链接与原文复制。
assistant 文件引用、工具标题/参数中的文件引用，以及 edit/write 的文件名，仅在解析为项目内现存普通文件时
链接到 `file://<绝对路径>`。规范化路径后检查项目边界，指向项目外的符号链接不生成文件链接；
路径中需要转义的字节使用百分号编码，`:42` 等行号后缀只保留显示，不放进文件链接。
链接保持原有颜色，仅 Markdown 链接文本和裸 URL 加下划线，文件名不额外加下划线。
链接识别在块物化时进行，流式文本变化会刷新当前块的链接；用户消息、diff 代码不额外识别链接。
工具原始输出继续保留原有排版，行内 Markdown 链接的 label 可点击；assistant 和 Task 最终结果的 Markdown 隐藏链接标记。
OSC 8 的协议白名单、控制字符与长度限制由框架统一执行。

Task 运行中与结束后共用标题：状态符、粗体 `Task`、两空格、text 色的 Agent 名、
两空格、muted 任务描述，最右为 `N tools · S.Ss`；数量为 1 时使用 `1 tool`。
描述按列省略并为统计让位；运行中按已收到的子工具调用计数，耗时每 100 ms 更新，结束后使用 TaskView 的最终统计。
子工具首行是 `⎿ ● Grep  "…"  6 matches`，后续子工具行与第一个子工具 bullet 对齐：
状态 bullet 按成功/失败着色、运行中为 accent spinner，名称加粗、参数 muted、统计 faint。
结束后保留子工具摘要，最终结果去掉首尾空白行；两者均存在时 comfortable 留一空行，compact 不留。
最终结果复用 assistant 的 MarkdownStream 与 Markdown、代码、表格渲染器，支持行内代码、强调、列表、
标题、代码块与链接；正文仍为 muted，结果行保持在子工具下方并沿用结果缩进（Unicode 4 列、ASCII 5 列）。
整个 Task 主体按渲染后的行数折叠，Markdown span 保留对应原文偏移，继续支持选择与链接。
历史 TaskView 的子工具步骤只保存 summary/is_error，回放忠实使用这些摘要，不能补出缺失的参数和统计。
Grep/Glob 统计在数量为 1 时显示 `1 match` / `1 file`。

`WebView` 的卡片名称为 `Web search` / `Web fetch`，参数显示搜索词或最终 URL，统计显示 HTTP 状态和耗时；
抓取另显示字节范围/正文总量、cached 和 truncated 标记。状态 0 表示尚未取得可显示的 HTTP 状态，应查看正文中的失败原因。
抓取默认显示行数摘要，Ctrl+O 展开后查看剩余元信息与正文，不再重复标题中的 URL。
搜索摘要和错误正文继续显示折叠预览；恢复历史不会重新联网，也不会重建分页缓存。

思考正文默认收起（`collapsed_rows = 0`），Ctrl+R 统一展开/收起已完成及正在流式生成的思考。
标题始终为 `✻`，不再使用 +/-；无正文或仅空白的思考不显示。
展开后的正文为 text_muted 斜体，缩进 4 列，第二列的 `│` 使用 text_faint；只有思考展开保留这条竖线。
历史没有思考耗时，继续使用回放投影的耗时；不伪造历史轮次尾行。

Ask 运行中和完成后共用 `Ask  <header>` 标题，仅状态符由 spinner 变为 `●`，不把问题摘要拼入标题。
开始事件尚无 header 时只显示 Ask；收到问答请求后按未完成调用的问题匹配卡片，填入 header 与 `⎿` 下的问题正文。
问题仅在结果首行出现一次。
下一行起选项按内容宽度同行排列，放不下则换行；过长单个选项内部折行。
选中项使用 background_selected 和 success 的 `✓ `，其他项使用 background_element 与 text_muted；
自由输入作为选中的 Other 标签，取消状态放在标题统计处。问题仍开放时原模态对话框和键位保持不变。

每次 `TodoView` 都在对应 Plan 工具下保留列表快照，不再随侧栏宽度迁移或额外追加 plan complete 系统块。
完成项为 success `☒` 与 muted 正文；当前项为 accent `☐` 与粗体正文；待办为 muted `☐`，
已放弃项保留 muted 复选框及删除线。Plan 和 Ask 完整显示，不参与 Ctrl+O 折叠。
SidePanel 同步最新计划，`/new` 清空，恢复会话依靠最后一次 TodoView 重建；侧栏隐藏不会删除对话内列表。

### 子会话 Pane

Shell 持有 `std::vector<Pane>`：索引 0 恒为主会话，task 的子事件首次到达时按父调用 id 建一个子 Pane
（标题 `task · agent`，记录子会话 id），之后该子会话的事件同时喂给这个 Pane 的 Transcript。Pane 的
Scrollback 由 `ScrollFrame` 统一持有，`show()` disown 旧区、adopt 新区；鼠标处理器每个 Pane 一个，
切换时解绑旧的、绑定新的。Ctrl+A / `/agents` 打开 Panel 列出主会话与全部 task；子 Pane 下输入框禁用，
Ctrl+C 或再次切换回到主会话。

活动期之外的子会话没有 Pane：恢复父会话时只还原 task 块，用户从 `/agents` 选中某个 task 时才用
`session.history` 分页读取子会话记录并投影进新 Pane。浏览是只读查询，不修改子会话记录或 updated。子 Pane 下提交输入被拒并保留草稿。

有子 Pane 时 HeaderBar 下方出现 AgentTabs，属于基础布局并从工作区扣除两行高度。
显示 `main` 与各 `task · agent`，每项带运行 spinner / 成功勾 / 失败或中断叉。
当前 Pane 标签为 text，下一行用 primary 的 `─` 标记，其他标签为 muted；右侧 `ctrl+a switch`。
宽度不足时裁剪长标签并移动可见任务窗口，保持 main 和当前选择可见。
Ctrl+A 仍打开现有 picker；点击可见标签通过现有 Runtime 鼠标绑定直接切换。
未选择过的历史 task 不提前创建 Pane，但可在 AGENTS 节查看其已保存的耗时和结果状态。


## 6. 状态、浮层与权限

HeaderBar 左侧为 primary 粗体品牌与首条用户输入的首行（未输入时为 muted 的 new session）。
右侧为权限模式 Chip、模型 Chip 与 muted 的分支；unrestricted 用 error，plan 用 accent。
用户隐藏侧栏或终端少于 80 列时，右侧保留模型 Chip 与上下文百分比，移除模式和分支；
标题、模型与长分支都按显示列宽裁剪，避免左右重叠。

HintBar 左侧是缩成 `~` 的项目路径；右侧按 key=text、description=text_muted 绘制，项目间用 text_faint 的 ` · `。
空闲显示 ctrl+p commands、ctrl+t sidebar、shift+tab mode；忙碌显示 esc interrupt，
有排队输入时加 ↑ recall queued，有子 Pane 时加 ctrl+a agents。Panel/Completion 打开时显示
enter select、esc close；Settings 显示 enter save、esc revert。
ModelPicker 显示 `enter switch · → open provider · a add custom · esc close`；
登录等待显示 `c copy link · o open again · esc cancel`，失败显示 `enter retry · esc back`；
Go 清单显示 `enter add & switch · r refresh · esc back`。这些提示也出现在对应浮层底部边框。
审批显示 y allow、n deny、esc deny，
问题显示 enter confirm、esc cancel。欢迎页有最近会话且输入为空、队列为空、空闲时显示
`↑↓ pick session · ctrl+p commands · ctrl+c quit`；ASCII 集使用对应方向字符。其余时候沿用普通空闲提示。
上下文与计划进度不再占用底栏。

SidePanel 留一行上边距和两列左右留白，章节标题为大写 muted，comfortable 章节间空一行：

| 节 | 内容 |
| --- | --- |
| SESSION | 首次用户输入标题，最多两行；会话 UUIDv7 创建时间转本地 HH:MM、回放和实时 user 轮数（窄栏自动折行） |
| CONTEXT | 标题右侧百分比、primary/border 用量条、简写的 used / limit tokens；达到压缩阈值用 warning |
| PLAN | 仅收到 TodoView 后显示；done/total；与 Plan 卡片共用图标表：完成 `☒` success、正文 muted；当前 `☐` accent、正文加粗；待办 `☐` 与正文均 muted；ASCII 使用 `[x]` / `[ ]`，按图标宽度缩进正文 |
| AGENTS | 位于 PLAN 与 CHANGED 之间，仅有任务时显示；左侧状态图标和 Agent 名，右侧耗时，运行中随动画刷新 |
| CHANGED | 当前会话已成功 edit/write 的相对路径和累计 +N −M（分别为 success / error 色，均不加粗）；最多 8 文件，其余 +K more |
| MCP | 配置的服务器逐项显示名称和 ready/connecting/failed，分别用 success/warning/error 状态符 |

底部固定 text_faint 的 DAgent 版本；路径与分支分别交给 HintBar 和 HeaderBar。计划卡片始终保留在 Transcript 中。
恢复会话通过已有历史投影恢复标题、轮数、计划与文件增删统计，不访问工作树猜测会话改动。

Panel 最大宽 76 列、高度不超过终端 60%，由标题、可选内存过滤、列表、过滤计数与底部提示组成。
高度按实际内容计算，查询为空时不为过滤计数预留行，列表末尾不额外留空行。
每个 Row 可带 group；按组的首次出现顺序归组，组头为大写 muted，不可选择，除首组外前置一空行。
过滤后没有任何行的组不显示。↑/↓ 仅在可用数据行间循环，滚动按包含组头与空行的实际显示行定位。
过滤仍为 label 的大小写不敏感子串匹配；label 中匹配的字素使用 primary 加粗。
选中行填充 background_selected，左侧 primary `▌`（ASCII 为 `|`）。

列表列为 label、可选 middle、alias、right：label 使用 text，后三者 muted；right 右对齐，
alias 与 right 保留独立列宽，right 按当前行内容占用宽度。middle 跟随 label 并使用扣除别名和右列后的剩余宽度，
空间不足时省略；所有裁剪按终端显示列宽。
启用过滤且查询非空时，在 hint 上方显示 muted 的 `N of M <noun>`；默认 noun 为 items，命令面板为 commands。
不支持过滤的 Panel 无搜索框和计数。会话列表和文件候选的 I/O 不发生在渲染线程。
`/memory` 的 Global / Project 使用实际分组；`/agents` 的 main 保留会话 id 前八位，子项 middle 显示任务描述，
实时取自子会话 turn_started，历史取自 TaskView，不再显示易重复的 UUID 时间前缀。

Ctrl+P 标题为 `Commands`，按已有 Session、Permission、Transcript、View 等命令分类分组。
每项同时显示 slash alias 和 key binding（若存在），忙碌时仍按原 enabled 条件禁用。
hint 为 `↑↓ move · enter run · esc close`；ASCII 图标集使用对应方向字符。

模态变暗完全在应用层 Page 完成：Panel、ModelPicker（含登录）、SettingsDialog、ApprovalDialog/Question、ModelDialog 任一个可见时，
重绘完整基础层，再通过现有 Surface::at/put 把每个非占位单元格的前景色向页面背景混合 60%。
字符、字重及背景均保留，宽字素按原宽度写回；先重画再混色，避免增量帧累积变暗。
之后框架照常绘制各浮层，Toast 和 Completion 不触发变暗，Toast 自身也不被混色。
indexed 色先转 RGB，再走既有终端输出量化；transparent 不改变任何 default 背景。
透明页面的混色目标优先取终端报告的 OSC 11 背景，终端未报告时使用 background_panel 作为近似。
没有新增或修改冻结框架的接口。

ToastStack 保持右上角、最多三条、新通知在上、五秒到期；每条为面板底色的圆角框。
边框与前导图标按 info / success / warning / error 使用 info / success / warning / error 色，正文始终为 text。
图标来自统一表：`● ✓ ! ✗`，ASCII 为 `i v ! x`。框架 Notice 只有 info/warn/error，
应用 ToastStack 自有 severity 支持 success；设置保存成功使用 success。错误 Event 已永久进入 Transcript，仍不重复弹 toast。

ApprovalDialog 使用风险色圆角框：一般命令、network/read/write 为 warning，host access、protected write、
sensitive read 或 partially_executed 为 error。标题为加粗 `Approval needed`，两侧各留一个空格，随后是同色类别 Chip，
采用首项请求的 `COMMAND / NETWORK / READ / WRITE / SENSITIVE READ / PROTECTED WRITE / HOST ACCESS`；
其他不同 kind 的数量显示为 `+N`。摘要使用原 summary，只显示一次。

预览位于页面底色矩形中；命令复用既有 SyntaxRenderer 的 bash 高亮，并用 muted `$` 替代原代码装订线。
其他 code/diff/text 保留原渲染类型并使用同样底色。预览和详情共用 Scrollback，初始定位顶部，
按当前宽度的实际内容行数确定高度，不用空行填满；整个审批/问题框按内容收缩，保留原终端约 60% 的高度上限
（短终端沿用原最多 8 行、且不超出屏幕的上限）。超过上限时，PgUp/PgDn 与鼠标滚轮继续滚动预览。
详情左侧固定八列，label 为 muted，内容折行并保持对齐：
`cwd` 缩短 HOME 为 `~`，`mode` 保留原模式，`needs` 每项一行（kind、target 与原 request reason），
`reason` 为总体理由，仅提供会话授权选项时以 `session` 行显示完整 session_rule，
子 Agent 请求增加 `via task · <agent>`。无 d3 review 数据，因此没有 review 行。
保留完整宿主边界说明、远端请求自带的边界/理由文本、一次性说明、部分执行警告与各权限原因。

选项使用元素底色 Chip，key 为 primary 加粗，label 为 text：`y allow once`、
`a allow for this session`（仅有 session_rule 时）、`n deny`、`e deny and explain`。
120 列终端下四个 Chip 同行，窄宽度按显示列折行；完整 session_rule 在详情区折行。
←/→ 循环选择，primary 竖条标记当前项，Enter 确认；
y/a/n/e 仍直接执行，Esc 拒绝、Ctrl+C 中断保持原行为。框内底部为
`y a n e · ←→ choose · enter confirm · esc deny`，不提供 session allow 时省略 a。
反馈输入保留原提交/退出行为；底部单独预留 hint 和边框行。

Question 使用同一骨架，primary 边框、原 header 标题和原选项列表；审批与问题标题两侧均留空格（`╭─ Title ─…`）。
问题正文按内容行数占位，正文与选项间只留一行间距。数字/上下键选择、Enter 确认、
多选空格、Other 自由输入、Esc 取消保持不变，unrestricted 确认默认取消，Shift+Tab 仍可穿透循环权限。
权限与问题互斥。`/permissions` 复用 Panel 显示内存授权，Enter 撤销；空列表只显示不可选提示。
unrestricted 的输入框模式 Chip 使用 error，plan 使用 accent；普通轮次尾行统一使用 text_faint。

### 欢迎页

WelcomeView 复用原六行块字标并使用 primary；内容栏不足 60 列时为 `◆ DAgent`，ASCII 时为 `* DAgent`。
下面的 Chip 按空间自动换行，遵循 filled / outline：缩为 `~` 的项目路径、git 分支（仓库外省略）、
模型、权限模式（与 HeaderBar 同色）、`● MCP ready/total`、`N memories`。
MCP 未配置时省略；全部 ready 为 success，失败为 error，其余为 warning。记忆数量来自已有 memory.list，零时省略。
外观、模型、模式、MCP 快照、分支的变化更新欢迎页，无额外周期轮询。

`RECENT SESSIONS` 从已有 session.list 返回结果中选取最多五个有标题的非当前会话，跳过空标题会话；
`/sessions` 仍保留空会话。标题在左，相对时间在右且为 muted：
`2h ago`、本地日期的 `yesterday`、更早为 `Oct 18`。无符合条件的会话时整节省略；节尾 muted `/sessions all sessions`。
输入框为空、无排队输入且空闲时 ↑/↓ 循环选中最近会话，选中行使用 background_selected 和 primary `▌`；
Enter 复用 /sessions 的 session.resume 路径。输入任意文字或粘贴清除选择并交给原 PromptInput 编辑。
短终端可用 PgUp/PgDn、Home/End 或鼠标滚轮查看余下内容，改变会话选择时让该行可见。

`TIPS` 的 key 为 text、说明为 muted：`@ attach a file`、`$ run a skill`、
`shift+tab cycle permission mode`、`ctrl+g read-only plan mode`、`/settings theme and background`。
unrestricted 模式继续显示其权限说明。comfortable 保留节间空行，compact 去掉这些空行。
欢迎页首次发送消息后消失；恢复会话（包括恢复空会话）直接进入 Transcript。

符合条件的子权限请求由父模型审阅，显示 `Reviewing subagent permission`，不打开人工模态框。
决定以持久 Notice 保留来源、范围与理由；历史 `permission/parent_review` 投影为相同的审计提示。
路由条件、失败和降权行为见 [权限指南](../guide/permissions.md)。

## 7. 退出与错误

`/exit`、连续 Ctrl+C 或进程信号都走同一退出流程：请求当前 Run 取消，Runtime 退出并还原终端，随后前端发 `backend.shutdown`
等待后端收尾（宽限 10 秒，超时回收自己创建的后端），最后在普通终端打印恢复命令。后端连接意外结束时界面显示错误并退出，
不重连、不重发输入。

单轮 failed、denied、limit 或模型连接失败只结束本轮，界面仍可继续接受输入。正常退出返回 0，进程信号退出返回 130，
启动或通信失败返回 1。


## 模型选择与登录

`/model` 或 Ctrl+M 打开 `ModelPicker`，仅空闲时可用；忙碌时命令面板禁用，直接输入 `/model` 提示不可用。
Ctrl+M 需要终端提供可区分的扩展按键编码；传统终端把它与 Enter 编成相同字节时使用 `/model`。
浮层居中、圆角 active 边框、标题 `Model`，最大约 100×24，随终端缩小。终端少于 80 列时取消详情栏。
浮层填色、选中元素色、`▌`、匹配字素的 primary 加粗、分组与过滤计数复用应用层 Panel；框架不变。

左侧依次为 CONFIGURED、PROVIDERS 和 `+ Add a custom model`。配置名来自 `model.list`：
右侧显示 current 或认证来源 key / login / local；已知未登录/失效的登录模型优先用 warning 显示 needs sign-in。
打开时请求纯本地 `provider.status`，等待显示 `…`，失败在提供方行和详情中显示错误，Enter 重试。
OpenCode Go 行未登录为 sign in，needs_sign_in 为 warning 的 expired · sign in；已登录为 `N models ›`，
尚未取得清单时只显示箭头。N 来自本前端已取得的清单，不为显示数量额外联网。

右侧展示配置名、via OpenCode Go 或 URL host，以及对齐的 kind、endpoint（去掉 userinfo/query/fragment）、
context、max output、reasoning（仅 openai-chat / ollama，取 send_reasoning_content）、auth。
context 达到百万时以 M 显示，最多两位小数并去掉末尾零（1M、1.05M），低于百万按整千显示（262k、500k）；
max output 显示带千位逗号的精确值（32,768）。这些格式仅用于模型限制，侧栏和活动行的用量格式不变。
已浏览 Go 清单后，同名配置显示已知 price（美元/百万输入输出 token，均为零显示 free）。
登录模型附 SIGN-IN：success 的 signed in、工作区和 `expires <Mon D> · refreshes itself`，或 warning 的
not signed in 与 `/model` 提供方行的登录引导。底部注明 `saved in home/config/models.json`。
提供方详情展示状态、工作区、到期日、Go 订阅链接和 Enter 的行为；添加行解释自定义表单。

↑/↓ 移动；输入过滤（a 保留为快捷键）；Enter 配置项调用现有 `session.select_model`；
Enter 或 → 提供方打开登录/清单；Enter 添加行或任意位置 a 打开原 ModelDialog；Esc 关闭。
ModelDialog 仍为七步表单：协议、配置名、base URL、模型 ID、API key（也可填 env:VARIABLE）、
最大输出和上下文窗口。协议默认值来自 model.list；Enter/Tab 前进，Shift+Tab 返回，Esc 取消。
提交经 model.add 保存并切换，密钥仅为该请求的 write-only 字段。

### OpenCode Go 登录

未登录或失效时发 `provider.login.start`，显示 `Sign in to OpenCode Go`。收到授权链接后前端尝试打开浏览器；
启动成功显示 `Your browser was opened to authorize DAgent.`，无法启动则显示 `Open this link to authorize DAgent:`。
链接用 primary 并按列省略；用户码复用 Chip，遵循 filled/outline；等待行使用统一 spinner 与 Unix 秒 expires_at 倒计时，
另有 `No Go subscription yet? https://opencode.ai/go`。c 经现有剪贴板复制完整链接并 toast；o 再次打开。

`provider.login.wait` 异步等待；成功 toast `Signed in to OpenCode Go` 并直接进入清单。
拒绝、到期或错误以 error 显示，Enter 重新开始，Esc 返回。
Esc / Ctrl+C 发送 provider.login.cancel 并返回选择器；即使 start 尚未返回，也会在迟到结果到达后取消该 login_id。
界面代次隔离已关闭/已替换页面的回调，避免迟到结果重新打开浮层。

### OpenCode Go 模型清单

已登录时选择提供方或授权成功后发 `provider.models.list`。标题 `OpenCode Go`，搜索字段下显示
`signed in · <workspace> · expires <Mon D>`。行以模型 ID（如 deepseek-v4.1-flash，不含 opencode-go/ 前缀）为标签，
上下文简写右对齐，价格和右侧状态使用固定列对齐；上下文格式与模型详情一致，每百万 token 输入/输出价格免费时为 free。
搜索同时匹配 ID 与显示名称，显示名称不占用列表列。已配置为 added。
available=false 的 Anthropic 项用 muted / unavailable，方向键跳过，无法选中。
加载中显示加载行，查询或添加失败显示 error 行；r 以 refresh=true 重新获取，Esc 返回选择器。
页脚说明 context、price 单位、models.dev 来源、缓存年龄（cached 41s ago / 2h ago，少于 10 秒为 cached just now）；
stale 标注 stale / offline cache。

Enter 发 `provider.model.add`，由后端保存缺少的 `opencode-go/<model>` 条目并切换；已存在时不覆盖。
成功 toast `Switched to opencode-go/<model>` 并关闭；保存成功但未切换时保留错误，刷新配置列表后可以重选。
切换保留会话 ID 和历史，更新 HeaderBar、输入框与 Context；新条目不会使历史标签变化。
本前端仅持有公开状态、清单和认证来源，不显示令牌或请求头。

使用中提供方认证失效的受控错误仅在 UI 投影替换为：
`OpenCode Go sign-in is no longer valid. Open /model → OpenCode Go to sign in again.`
`dagent run` 和后端保存的错误文案不变；再次打开选择器重新读取 provider.status。

本阶段仅构建验证；真实浏览器批准、models.dev 清单、添加切换、对话和失效状态由用户验收。

## Skill selection

`/skills` opens the skill catalog and discovery diagnostics. Selecting a skill inserts `$name` without submitting; typing `$` opens skill completion. Both reuse the existing application panel/completion widgets and fetch metadata through `skills.list`. Skill tool results use a dedicated card projection. See [skills](skills.md).

历史加载各自拥有条目缓冲和分页游标；切换会话会取消旧加载，完成或失败即释放。主会话实时事件始终进入主文档，子事件按会话身份进入子文档。

phase 2a（会话区）、phase 2b（浮层与欢迎页）、phase 5a（配色）和 phase 5b（对话区）已实现。
phase 5b 仅构建验证，不添加测试代码或模拟入口；深浅主题、多终端宽度、ASCII/compact 与真实工具交互的 PTY 验收由用户执行。
