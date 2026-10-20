# 终端 UI 框架

DAgent 自带的终端 UI 框架。头文件在 `src/public/tui/`，实现在 `src/private/tui/`，构建为静态库
`dagent_tui`，私有链接 `dagent_base` 的终端判断原语。源码注释中的 `§N` 指本文章节。

框架于 2026-09-18 冻结；2026-10-20 的 TUI 改版 phase 4 解冻，扩展通用超链接和文档定位接口。
DAgent 在这些原语上搭建的实际界面见 [ui：应用层交互界面](ui.md)。

---

## 1. 概述

框架为 Agent 的终端界面提供底层原语：把终端当作一块单元格画布来管理、只重画变化的部分；
提供布局、浮层、几个基础控件、一个能容纳长对话的滚动文档，以及输入解码、快捷键和调度。

两条设计承诺：

- **业务线程永不被界面阻塞。** 界面只在一个渲染线程上运行，业务线程只能把更新投递给它。
- **界面静止时不输出、不唤醒。** 只有内容真的变了才出帧。

### 能力清单

| 能力 | 由谁提供 |
| --- | --- |
| 进入/退出全屏界面，异常退出时还原终端 | `Terminal` |
| 探测终端能力（真彩色、同步输出、kitty 键盘协议、背景色等） | `Runtime` 启动时握手，结果在 `Terminal::Caps` |
| 只重画变化的单元格，不闪烁 | `Surface` 与出帧 |
| 中文、emoji 等字符的正确显示宽度 | 字素模块 |
| 纵向、横向布局 | `Container` |
| 对话框、弹窗、toast 等浮层 | `LayerStack`、`Runtime::open_overlay` |
| 静态文本、转圈状态、通知、多行输入框 | `Text`、`Activity`、`Notice`、`InputBox` |
| 语义化主题，按终端背景自动选明暗 | `ThemeTokens`、`default_theme` |
| 长对话滚动区：流式追加、自动折行、折叠 | `Document`、`Scrollback` |
| 流式 Markdown、表格、diff、代码高亮 | `MarkdownStream` 与块渲染器 |
| 鼠标选择，复制到系统剪贴板 | `Scrollback`、`ScrollbackMouse` |
| 键盘、鼠标、粘贴输入 | `Decoder`、`EventRouter` |
| 单键快捷键与命令表 | `Keymap` |
| 定时器与动画 | `Runtime::after` / `every` |

### 不提供什么

框架只给原语，不给界面。会话视图、消息排版、对话框、补全弹窗、侧栏、命令面板，都由应用层
（`ui` 模块）用这些原语搭建；主题文件、快捷键配置的读取也属于应用层。

---

## 2. 结构

### 分层

共七层，上层依赖下层，下层不知道上层：

```
L7 运行时   Runtime · Keymap · ScrollbackMouse
L6 输入     Decoder · EventRouter · EventHandler · InputBoxHandler
L5 文档层   Document · Scrollback · MarkdownStream · 块渲染器
L4 控件     Text · Activity · Notice · InputBox · ThemeTokens
L3 布局     Widget · Container · LayerStack
L2 网格     Surface · Cell · Style · 字素与宽度
L1 终端     Terminal
```

| 层 | 头文件 | 实现 |
| --- | --- | --- |
| L1 终端 | `terminal.hpp` | `terminal.cpp`、`terminal_posix.cpp` |
| L2 网格 | `surface.hpp`、`grapheme.hpp` | `surface.cpp`、`present.cpp`、`grapheme.cpp` |
| L3 布局 | `layout.hpp` | `layout.cpp` |
| L4 控件 | `widget.hpp` | `widget.cpp`、`theme.cpp` |
| L5 文档层 | `document.hpp` | `document.cpp`、`wrap.cpp`、`markdown.cpp`、`syntax.cpp` |
| L6 输入 | `input.hpp` | `input.cpp` |
| L7 运行时 | `runtime.hpp`、`event_wait.hpp` | `runtime.cpp`、`keymap.cpp`；原生事件等待在 `terminal_posix.cpp` |

### 对象关系

一个运行中的界面大致是这样：

```
                 ┌──────────► Terminal
                 │
Runtime ─────────┼──────────► 根控件 LayerStack
                 │               ├─ 基础层 Container
                 │               │    ├─ Scrollback（持有 Document）
                 │               │    ├─ Activity / Notice
                 │               │    └─ InputBox
                 │               └─ 浮层：对话框、弹窗……
                 │
                 └─ 事件处理器：模态处理器、焦点处理器（InputBoxHandler）、
                                全局处理器（Keymap）、鼠标处理器（ScrollbackMouse）
```

- `Terminal` 和根控件由应用创建，`Runtime` 只引用它们。
- 容器拥有子控件；`Scrollback` 拥有它显示的 `Document`。
- **控件和事件处理器是两种对象。** 控件只管布局与绘制，不认识事件；处理器只管事件。两者的
  对应关系登记在 `Runtime` 上。

### 线程

调用 `Runtime::run()` 的线程就是渲染线程。控件树、Document、事件处理器都只在这个线程上操作，
框架不为它们加锁。其他线程只能调用 `post(fn)`（把函数交给渲染线程执行）和 `quit()`。

---

## 3. 运转方式

### 应用接入

```cpp
Terminal term;                                   // 进入全屏界面，析构时还原

auto body = std::make_unique<Container>(Container::Direction::vertical);
auto sb   = std::make_unique<Scrollback>();
auto box  = std::make_unique<InputBox>();
Scrollback* scroll = sb.get();
InputBox*   input  = box.get();
body->add({Sizing::flex, 1}, std::move(sb));     // 滚动区占满剩余空间
body->add({Sizing::content}, std::move(box));    // 输入框按内容高度
LayerStack root{std::move(body)};

Runtime rt{term, root};
InputBoxHandler edit{*input};
Keymap keys{rt};
rt.set_focus(&edit, input);                      // 按键先给输入框，光标跟随输入框
rt.set_global(keys);                             // 输入框不要的按键交给快捷键层
rt.on_caps([&](const Terminal::Caps& c) {        // 终端能力确定后选主题
    scroll->set_theme(default_theme(c.background));
});

// 业务线程里：
rt.post([&] { scroll->document().append_block(BlockKind::text, "hello"); });

rt.run();                                        // 阻塞到退出
```

### 一次按键怎样到达屏幕

1. 终端送来字节，`Decoder` 解成事件。
2. 键盘事件依次交给模态处理器、焦点处理器、全局处理器，谁消费就停在谁；鼠标事件按坐标交给
   被点中控件的鼠标处理器。
3. 处理器修改控件，控件标记自己需要重画。
4. 渲染线程出帧：必要时重新布局，只重画标记过的控件，只把变化的单元格写到终端。

### 一次业务更新怎样到达屏幕

业务线程 `post(fn)` 后立即返回。渲染线程被唤醒，按顺序执行排队的函数（例如往 Document 追加
一段流式文本），然后照上面第 4 步出帧。

### 出帧节奏

- 有变化才出帧；变化密集时最多每 16ms 一帧（`Runtime::Options::min_frame`），把连续变化合并。
- 没有输入、没有更新、没有定时器时，渲染线程一直睡着。
- 终端尺寸变化时整屏重新布局、重画。

---

## 4. 模块

### 4.1 终端（L1）

`Terminal` 管理终端会话：构造时进入全屏界面（备用屏、raw 模式），析构时还原。进程收到 SIGINT、
SIGTERM、SIGHUP 退出时同样会还原。stdout 不是终端时不进入界面模式。

它还提供：查询尺寸、写字节、开关鼠标上报、写系统剪贴板（OSC 52）。
当前终端的能力记录在 `Terminal::Caps`：真彩色、同步输出、鼠标、括号粘贴、
kitty 键盘协议、字素宽度模式、背景色。

`terminal.cpp` 保留能力判断、ANSI 序列与模式状态；`terminal_posix.cpp` 实现原生终端操作。
公共头只持有不透明的 `Native`，不暴露 termios、信号管道或其 fd。stdin/stdout 的终端判断分别调用
`base::stdin_is_terminal` / `base::stdout_is_terminal`。

构造先探测能力，再分配 Native、创建 self-pipe；成功时发布写端并安装处理器，失败仍降级运行。
随后在 stdin 为终端且 tcgetattr 成功时保存原设置、进入 raw 模式，再进入界面模式，最后登记全局实例和 atexit。
`restore` 先用原子标记去重，退出已开启的界面模式，再恢复 termios；它不卸载处理器或关闭管道。
析构先 restore，再摘除信号写端并还原调用方的处理器，随后关闭 self-pipe 两端、清除全局实例，最后释放 Native。
信号处理器仍只写 self-pipe，并保存/恢复 errno；安装时保留 SA_RESTART。

### 4.2 网格与出帧（L2）

`Surface` 是一块单元格网格，每格存一个字符、它的宽度和样式（`Style`：前景色、背景色、粗体、
斜体、下划线等，以及 `uint32_t hyperlink`）。控件都画在网格上，而不是直接往终端写字符串，所以相邻、重叠的控件能正确
叠在一起。

框架保留上一帧和本帧两块网格，出帧时逐行比较，只把变化的部分写到终端。支持同步输出的终端上
整帧一次性显示，不会闪烁。真彩色终端输出 24 位色，其他终端输出 256 色。

**颜色回退（phase 5a）。** `Color` 增加 `int16_t fallback_256 = -1`；
`Color::rgb(r, g, b, fallback = -1)` 可携带 0–255 的 xterm 索引。
仅 RGB 且终端无 truecolor 时，出帧器优先输出该索引；-1 沿用色立方与灰阶的最近距离量化。
真彩色忽略索引，indexed/default 色行为不变。Color/Style/Cell 的相等比较包含该字段，
所以单独修改回退索引也能触发重绘。重新混合生成的 RGB 默认不携带固定索引。
框架不读取主题 JSON；应用层负责解析、校验颜色对象。

**超链接接口（phase 4）。** `Style::hyperlink` 默认为 0（无链接）。渲染线程调用
`intern_hyperlink(std::string_view uri)` 获取进程内稳定、去重的非零 id，
`hyperlink_uri(uint32_t id) noexcept` 返回相应 URI；0 或不存在的 id 返回空视图。
表以独立的进程级存储保存 URI，生命周期与进程相同，不随字素 `intern_reset()` 清除，
所以跨文档缓存、Surface 子视图和双缓冲复制后 id 仍然有效。接口仅用于渲染线程。
白名单只允许 http、https、file scheme（不区分大小写）；超过 2048 字节或包含 C0、DEL、C1
控制码点的 URI 返回 0，不存表、不输出。框架不解析应用文件路径、不决定链接颜色或下划线。

Style/Cell 相等比较包含 hyperlink，因此只有链接目标改变也会置脏并输出；仍使用原有脏行跳过、
首尾变化列定位和宽字符占位规则。`render_frame` 在链接变化时发
`ESC ] 8 ; ; <uri> ESC \`，切入无链接单元格前、每次行写出结束时发
`ESC ] 8 ; ; ESC \` 关闭链接；因此下一次 CUP 或帧末光标定位前链接已经关闭。
SGR 增量逻辑独立保留，输出不依赖 OSC 8 能力探测；忽略 OSC 8 的终端仍显示相同文字和颜色。
`FrameOptions`、`Surface::put/text/fill/view`、`present` 的参数未改变。


### 4.3 字素与宽度（L2）

终端里「一个字符占几列」并不简单：中文占 2 列，组合符号占 0 列，一个 emoji 可能由好几个码点
组成。这个模块把 UTF-8 文本切成用户眼中的一个个字符（字素簇，Unicode UAX #29），并给出每个
字符的显示宽度（Unicode 17.0 数据）。

终端支持时，框架会开启字素宽度模式（mode 2027），让终端和框架按同一套规则算 emoji 的宽度。

### 4.4 布局与浮层（L3）

`Widget` 是所有控件的基类，只关心三件事：想要多大、被分到哪块区域、怎样画到网格上。它不处理
事件。

`Container` 把子控件沿竖直或水平方向排开，每个子控件带一个约束：固定大小（`fixed`）、按内容
（`content`）或按权重分剩余空间（`flex`），可以设最小、最大值。空间不够时按顺序压缩。

`LayerStack` 用作根控件：一个基础层，上面叠若干浮层。浮层可以居中（对话框）、放右上角（toast）、
贴在某点上方（补全弹窗，放不下就翻到下方）或放在某点。浮层不透明；浮层关闭或移动后，露出来的
部分自动补画。它也负责鼠标命中：给定屏幕坐标，找出该位置最上层的控件。

### 4.5 控件与主题（L4）

| 控件 | 用途 |
| --- | --- |
| `Text` | 多行静态文本 |
| `Activity` | 转圈符号加当前动作；文本为空时不占位 |
| `Notice` | 单行通知，分 info / warn / error |
| `InputBox` | 带边框的多行输入框，提供插入、删除、移动光标等编辑操作 |

控件只提供内容和编辑操作，不处理按键；按键到编辑操作的翻译在输入模块（`InputBoxHandler`）。

**主题令牌。** 控件和渲染器都不写死颜色，而是引用 `ThemeTokens` 里的语义样式，例如 `text`、
`primary`、`error`、`border`、`diff_added`、`syntax_keyword`、`markdown_heading`、`selection`。
换主题就是换一组令牌；改动令牌后要递增其中的 `epoch`，已缓存的渲染结果才会刷新。框架内置暗色、
亮色两套，`default_theme(背景色)` 按终端背景的明暗自动选择。

phase 5a 的 ThemeTokens 字段按组如下（均为 Style，另有 epoch）：

| 类别 | 字段 |
| --- | --- |
| 文字与强调 | text、text_muted、text_faint、primary、accent |
| 边框与背景 | border、border_active、background、background_panel、background_element、background_selected |
| 状态 | success、warning、error、info |
| Diff | diff_added、diff_removed、diff_context、diff_hunk、diff_added_bg、diff_removed_bg |
| 语法 | syntax_keyword、syntax_string、syntax_comment、syntax_number、syntax_function、syntax_type |
| Markdown | markdown_heading、markdown_code、markdown_link、markdown_quote |
| 拖选 | selection |

text_faint 为第三文字层，background_selected 是选中行底色；diff_added_bg / diff_removed_bg 的颜色位于 bg。
默认 DiffRenderer 在增删文字样式上叠加对应底色。SyntaxRenderer 的默认底色改为页面，gutter 使用 text_faint，
让注释的第三档颜色落在页面上；应用仍可替换具体背景。
内置 dark_theme / light_theme 提供完整 RGB 值：error 无 bold，markdown_heading 为 text + bold，
syntax_comment 为 text_faint + italic，selection 为页面色文字和 primary 底色。
新增令牌与 fallback 字段均为通用渲染能力，预设名字、自定义 accent 对比度调整、背景推导和设置交互留在 ui/app。

### 4.6 文档层（L5）

用来显示对话记录这类又长、又在不断增长的内容。

**Document** 由一串块组成，每块保存原文和类型：普通文本、工具输出、diff、代码、Markdown、表格。
块可以流式追加、整体替换、折叠或删除尾部。屏幕上的行是按当前宽度从原文算出来
的，窗口变宽变窄时自动重新折行。折行照顾中文：汉字之间可以断行，句读标点不会落到行首。

**Scrollback** 是显示 Document 的控件，只渲染看得见的部分，内容再长也不变慢。滚动位置跟着内容
走而不是跟着行号走：重新折行时，正在看的那段不会跳走。停在底部时新内容到达会自动
跟随，往上翻着看时则保持不动。

**块渲染器** 按块类型决定显示效果，应用可以替换：

- Markdown：标题、列表、引用、分隔线、粗体、斜体、行内代码、链接；标记符号隐藏，只显示效果。
- 表格：按列对齐，放不下时收缩列宽。
- diff：增行、删行、hunk 头分色。
- 代码：按语言高亮，支持 C/C++、Python、JavaScript/TypeScript、JSON、Bash、Go、Rust。

**MarkdownStream** 边接收模型的流式输出，边把 Markdown 拆成块：段落、代码块、表格各成一块。
所以流式输出的过程中，代码高亮和表格也能正确显示。

**选择与复制。** Scrollback 维护一个选区，复制出来的是原文（Markdown 源码），不含折行产生的
换行。窗口宽度变化后，选区仍然覆盖原来那段内容。

**定位与可见性接口（phase 4）。** `Line::reordered` 默认为 false；渲染器将源片段重排到不同显示行时
在每行设为 true，并继续为内容 span 提供准确 `src`、为装饰提供 `k_no_src`。
`Document::row_of` 对这种块先按 span 的源字素寻找锚点，找不到时回退到已有行首偏移定位；
行首 offset 仍应递增，原有块保持二分定位路径。用于左右并排的内容时，各侧的 span 各自映射原文，
空白补行、分隔符与省略号为无源装饰；复制仍按 Document 原文选区处理。
Markdown TableRenderer 现在也为内容 span 保留源偏移，去空白、展开 tab、转义和隐藏行内标记后
仍指向原文；边框、补空白与省略号保持无源。

`Document::block_visible(uint64_t id, size_t first, size_t count) const noexcept` 使用最近一次
`begin_frame` 的前缀和，查询块的实际内容行（不含上边距）是否与指定窗口相交；不存在或无内容返回 false。
`Scrollback::block_visible(uint64_t id) const noexcept` 查询最近绘制视口中的同一关系。
它们不触发物化，块查找为二分，供应用显示与当前内容有关的提示。

布局接口未改变。应用可用 BlockRenderer 装饰器选择测量宽度、在物化行前插入无源空白 span，
使同一全宽 Scrollback 中的各类块拥有不同的阅读宽度；测量和渲染必须采用相同策略。
Document 仍只物化可见块、保留增量计数和窗口外驱逐，Surface 仍按脏行比较帧。


### 4.7 输入（L6）

`Decoder` 把终端送来的字节解成事件：文本、按键（含 Ctrl / Alt / Shift 组合与功能键）、鼠标、
粘贴、窗口尺寸变化、焦点变化。终端支持 kitty 键盘协议时，能区分 Ctrl+I 与 Tab 这类传统上分不开
的按键。协议私用区中的 KP_0–KP_9 与小键盘运算符映射回普通文本，小键盘方向/翻页/首尾键映射到对应
命名键；其它未识别功能键丢弃，不能把私用区码点当文本插入。认不出的转义序列整体丢弃，不会以乱码混进输入框。

`EventRouter` 决定键盘事件交给谁，顺序固定：**模态处理器（后压入的优先）→ 焦点处理器 → 全局
处理器**，第一个消费的为止。打开对话框时把它的处理器压入模态栈、关闭时弹出，按键归属随之切换，
不需要写条件判断。

`InputBoxHandler` 把按键翻译成输入框的编辑操作。Enter、Esc 等键它不消费，提交、补全由应用决定。

### 4.8 运行时（L7）

`Runtime` 把以上各层串起来，是应用直接打交道的对象。

Runtime 用 `unique_ptr<EventWait>` 持有等待对象，原生实现与 Terminal 位于同一个 `terminal_posix.cpp`，
不通过公共接口传递 fd。EventWait 借用 Terminal，只负责 poll、输入读取及 wake pipe；超时计算、事件消费与渲染仍由 Runtime 控制。
wake pipe 仍使用 NONBLOCK/CLOEXEC，创建失败 terminate。Runtime 析构先删除 inbox 节点，再销毁 EventWait、关闭 wake pipe 两端。

每轮先清理已取消的定时器条目并计算超时，再等待；EINTR 回到外层循环，其他等待错误设置退出标记并结束循环。
正常返回（含超时）依次处理信号、wake、输入，再 apply_inbox、resize、定时器、Esc、握手与渲染。
signal/wake 只响应 POLLIN；stdin 响应 POLLIN/HUP/ERR/NVAL，每次仍读 4096 字节，EINTR 重试，
EOF 或非 EAGAIN 错误请求退出，EAGAIN 不产生输入。已设置退出标记时仍按原位置执行本轮后续处理。
跨线程 wake 仍先 exchange `wake_pending_` 去重，再写唤醒字节；排空时先清标记，再循环读取，保留 EINTR 重试。

| 接口 | 作用 |
| --- | --- |
| `run()` / `quit()` | 进入主循环 / 请求退出 |
| `post(fn)` | 从任意线程把更新交给渲染线程 |
| `set_focus` / `set_global` / `push_modal` / `pop_modal` | 键盘路由 |
| `open_overlay` / `close_overlay` | 打开、关闭浮层；打开时可接管键盘与光标，关闭时恢复 |
| `bind_mouse` / `unbind_mouse` | 为控件绑定鼠标处理器 |
| `after` / `every` / `cancel` | 定时器；动画用 `every`，回调返回 false 即停止 |
| `on_caps(fn)` | 初次终端能力确定时回调；随后收到 OSC 11 背景通知时也回调，应用据此更新跟随主题 |
| `set_clipboard(text)` | 写系统剪贴板 |

**能力握手。** `run()` 启动时向终端发一组查询：是否支持同步输出、字素宽度模式、kitty 键盘协议，
以及背景色是什么。握手不耽误首帧，最多等 1 秒，结果确定后回调 `on_caps`。

**鼠标。** 按下后的拖拽一直交给按下时的处理器，即使指针移出了控件；有模态浮层时，浮层外的点击
交给这个浮层处理，不会穿透到下面。

**Keymap** 是快捷键与命令层，作为全局处理器安装。应用先注册命令（id、标题、分类、执行函数、
可用条件），再把单个组合键绑定到命令，例如 `"ctrl+p"` 或 `"shift+tab"`。`commands()` 列出全部命令，可以直接作为命令面板的数据源。

**ScrollbackMouse** 给 Scrollback 加上鼠标交互：拖拽选择、双击选词、三击选行、滚轮滚动，松开时
自动复制到剪贴板。使用前应用要先用 `Terminal::set_mouse(true)` 开启鼠标上报。

`ScrollbackMouse` 要在 `Runtime` 之前销毁；Keymap 不持有 Runtime。
