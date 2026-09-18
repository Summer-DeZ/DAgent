# 终端 UI 框架

DAgent 自带的终端 UI 框架。头文件在 `src/public/tui/`，实现在 `src/private/tui/`，构建为静态库
`dagent_tui`。源码注释中的 `§N` 指本文章节。

框架已冻结（2026-09-18）：只修缺陷，不改公开接口。

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
| 快捷键、leader 键、命令表 | `Keymap` |
| 定时器与动画 | `Runtime::after` / `every` |
| 暂离界面运行外部程序、Ctrl+Z 挂起 | `Runtime::run_external` / `suspend_process` |

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
| L1 终端 | `terminal.hpp` | `terminal.cpp` |
| L2 网格 | `surface.hpp`、`grapheme.hpp` | `surface.cpp`、`present.cpp`、`grapheme.cpp` |
| L3 布局 | `layout.hpp` | `layout.cpp` |
| L4 控件 | `widget.hpp` | `widget.cpp`、`theme.cpp` |
| L5 文档层 | `document.hpp` | `document.cpp`、`wrap.cpp`、`markdown.cpp`、`syntax.cpp` |
| L6 输入 | `input.hpp` | `input.cpp` |
| L7 运行时 | `app.hpp` | `app.cpp`、`keymap.cpp` |

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

它还提供：查询尺寸、写字节、开关鼠标上报与焦点事件、写系统剪贴板（OSC 52）、暂时挂起和恢复
界面模式。当前终端的能力记录在 `Terminal::Caps`：真彩色、同步输出、鼠标、括号粘贴、焦点事件、
kitty 键盘协议、字素宽度模式、背景色。

### 4.2 网格与出帧（L2）

`Surface` 是一块单元格网格，每格存一个字符、它的宽度和样式（`Style`：前景色、背景色、粗体、
斜体、下划线等）。控件都画在网格上，而不是直接往终端写字符串，所以相邻、重叠的控件能正确
叠在一起。

框架保留上一帧和本帧两块网格，出帧时逐行比较，只把变化的部分写到终端。支持同步输出的终端上
整帧一次性显示，不会闪烁。真彩色终端输出 24 位色，其他终端自动换算成 256 色。

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

### 4.6 文档层（L5）

用来显示对话记录这类又长、又在不断增长的内容。

**Document** 由一串块组成，每块保存原文和类型：普通文本、工具输出、diff、代码、Markdown、表格。
块可以流式追加、整体替换、折叠、删除，也可以从头部裁掉旧块。屏幕上的行是按当前宽度从原文算出来
的，窗口变宽变窄时自动重新折行。折行照顾中文：汉字之间可以断行，句读标点不会落到行首。

**Scrollback** 是显示 Document 的控件，只渲染看得见的部分，内容再长也不变慢。滚动位置跟着内容
走而不是跟着行号走：重新折行或裁掉旧内容时，正在看的那段不会跳走。停在底部时新内容到达会自动
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

### 4.7 输入（L6）

`Decoder` 把终端送来的字节解成事件：文本、按键（含 Ctrl / Alt / Shift 组合与功能键）、鼠标、
粘贴、窗口尺寸变化、焦点变化。终端支持 kitty 键盘协议时，能区分 Ctrl+I 与 Tab 这类传统上分不开
的按键。认不出的转义序列整体丢弃，不会以乱码混进输入框。

`EventRouter` 决定键盘事件交给谁，顺序固定：**模态处理器（后压入的优先）→ 焦点处理器 → 全局
处理器**，第一个消费的为止。打开对话框时把它的处理器压入模态栈、关闭时弹出，按键归属随之切换，
不需要写条件判断。

`InputBoxHandler` 把按键翻译成输入框的编辑操作。Enter、Esc 等键它不消费，提交、补全由应用决定。

### 4.8 运行时（L7）

`Runtime` 把以上各层串起来，是应用直接打交道的对象。

| 接口 | 作用 |
| --- | --- |
| `run()` / `quit()` | 进入主循环 / 请求退出 |
| `post(fn)` | 从任意线程把更新交给渲染线程 |
| `set_focus` / `set_global` / `push_modal` / `pop_modal` | 键盘路由 |
| `open_overlay` / `close_overlay` | 打开、关闭浮层；打开时可接管键盘与光标，关闭时恢复 |
| `bind_mouse` / `unbind_mouse` | 为控件绑定鼠标处理器 |
| `after` / `every` / `cancel` | 定时器；动画用 `every`，回调返回 false 即停止 |
| `on_caps(fn)` | 终端能力确定后回调一次，通常在这里选主题 |
| `run_external(fn)` | 暂离界面运行外部程序（如 `$EDITOR`），结束后恢复 |
| `suspend_process()` | 挂起进程（Ctrl+Z），回到前台后恢复 |
| `set_clipboard(text)` | 写系统剪贴板 |

**能力握手。** `run()` 启动时向终端发一组查询：是否支持同步输出、字素宽度模式、kitty 键盘协议，
以及背景色是什么。握手不耽误首帧，最多等 1 秒，结果确定后回调 `on_caps`。

**鼠标。** 按下后的拖拽一直交给按下时的处理器，即使指针移出了控件；有模态浮层时，浮层外的点击
交给这个浮层处理，不会穿透到下面。

**Keymap** 是快捷键与命令层，作为全局处理器安装。应用先注册命令（id、标题、分类、执行函数、
可用条件），再把按键串绑定到命令，例如 `"ctrl+p"`、`"<leader> n"`。支持多键序列和 leader 键，
等待下一个键超时就放弃这个序列。`commands()` 列出全部命令，可以直接作为命令面板的数据源。

**ScrollbackMouse** 给 Scrollback 加上鼠标交互：拖拽选择、双击选词、三击选行、滚轮滚动，松开时
自动复制到剪贴板。使用前应用要先用 `Terminal::set_mouse(true)` 开启鼠标上报。

`Keymap` 和 `ScrollbackMouse` 都要在 `Runtime` 之前销毁。
