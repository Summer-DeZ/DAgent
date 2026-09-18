# 终端 UI 框架

DAgent 自带的终端 UI 框架（`src/public/tui`、`src/private/tui`，静态库 `dagent_tui`）。
目标是支撑 OpenCode 级的 Agent 交互：流式 Markdown、工具调用输出、命令面板与对话框、
鼠标选择复制、快捷键、主题，同时保证业务线程永不被界面阻塞、静止界面零输出零唤醒。

本文描述**当前实现**。源码注释中的 `§N` 指本文章节。

**状态：已冻结（2026-09-18）。** 框架只修缺陷，不增删原语、不改公开接口；新的界面功能
一律在应用层（`ui` 模块，§13）基于现有原语实现。确需新增原语时，先修订本文并说明为何
无法在应用层完成。

---

## 1. 边界

框架提供**原语**：终端会话、单元格网格与差分、布局与浮层、几个基础控件、大内容文档层、
输入解码与路由、运行时调度。它**不提供**界面：会话视图、消息列表的具体排版、对话框、
补全弹窗、侧栏、主题文件格式都属于应用层。

框架与应用层之间只有两条通道：

- **控件树**：应用构建控件、绑定处理器，只在渲染线程上操作（§4）。
- **更新队列**：业务线程（Agent、网络、工具执行）经 `Runtime::post` 提交领域更新，
  由渲染线程上的应用代码映射成控件操作。业务代码不持有控件指针。

---

## 2. 分层与源文件

依赖只向下，每层的边界就是它**不做**什么。

| 层 | 职责 | 头文件 | 实现 |
| --- | --- | --- | --- |
| L1 | 终端会话：界面模式进入/还原/挂起、能力探测与记录、尺寸、信号、字节写出口。不做绘制决策 | `terminal.hpp` | `terminal.cpp` |
| L2 | 单元格网格、双缓冲、差分写出；UTF-8 解码、字素簇、显示宽度 | `surface.hpp`、`grapheme.hpp` | `surface.cpp`、`present.cpp`、`grapheme.cpp` |
| L3 | `Widget` 基类、`Container` 布局、`LayerStack` 层栈。不感知事件与焦点 | `layout.hpp` | `layout.cpp` |
| L4 | 基础控件 `Text`、`Activity`、`Notice`、`InputBox`；语义主题令牌 | `widget.hpp` | `widget.cpp`、`theme.cpp` |
| L5 | 文档层：块模型、增量折行、锚点、块渲染器、流式 Markdown、语法高亮、选择 | `document.hpp` | `document.cpp`、`wrap.cpp`、`markdown.cpp`、`syntax.cpp` |
| L6 | 输入：字节解码 → 事件 → 焦点链路由；`InputBox` 的按键翻译 | `input.hpp` | `input.cpp` |
| L7 | 运行时：更新队列、调度、定时器、浮层与鼠标分发、握手、挂起、快捷键 | `app.hpp` | `app.cpp`、`keymap.cpp` |

所有头文件都在 `src/public/tui/`，实现都在 `src/private/tui/`。

---

## 3. 贯穿全局的约束

**① 业务线程永不被界面阻塞。** 控件树与 Document 只属于渲染线程，框架里没有保护它们的锁；
业务线程与渲染线程之间只有一个更新队列，入队的临界区最坏 O(1)（§4）。

**② 静止界面零输出、零唤醒。** 帧节奏由唤醒驱动而不是固定频率轮询：按键与业务变更立即
出帧，最小帧间隔（默认 16ms）只用来合并突发。只有控件树真的失效（或尺寸、焦点变化）才出帧；
没有定时器、没有输入时 `poll` 无限期阻塞（§5.1）。

**③ source 是唯一真相，屏幕行是派生缓存。** 文档层的块只保存原文；折行、着色都是按宽度和
主题纪元缓存的派生结果。改变宽度能重折，折叠/展开能切换形态，裁剪按块整块进行；计数（对所有
块、零分配）与物化（只对可见块）分离；流式追加只做 `source += chunk`，折行从已定前缀增量进行，
代价是 O(最后一行)。滚动位置用锚点（块 id + 字节偏移），重折、裁剪、追加都不会让正在看的
内容跳走（§10）。

---

## 4. 线程模型与更新通道

渲染线程 = 调用 `Runtime::run()` 的线程，是全框架唯一的终端 I/O 者，也是控件树与 Document
的唯一所有者。

```cpp
void Runtime::post(std::function<void()> fn); // 任意线程
```

- 业务线程：节点在锁外分配，锁内只有一次尾插（两个指针写），然后写唤醒管道。入队即返回，
  fn 稍后在渲染线程上执行。需要完成通知的调用方在 fn 内 `set_value` 一个 promise。
- 渲染线程上调用（事件处理器、定时器回调、`render` 内）：直接执行，保持重入语义。
- 渲染线程每次被唤醒：锁内摘走整条链（头尾置空）→ 解锁 → 依序执行并释放节点 → 检查控件树
  是否失效。执行期间新到的 `post` 接在已空的链上并重新唤醒，不丢。

**为什么是侵入式链表而不是 `vector` + swap。** 后者的 `push_back` 只是摊还 O(1)：一次慢帧
期间队列能涨到十万级，那一次扩容要搬走全部已排队的 `std::function`，实测单次 `post` 达到
1.5ms。链表尾插与已积压的队列长度无关。

**唤醒**：等待点用管道而不是条件变量 —— 渲染线程要同时等 stdin、信号 self-pipe 与业务
更新，管道把三者统一进 `poll`。管道里已有未消费的唤醒字节时不再写（一千次 `post` 一次
`write`）；先入队后唤醒、先清标志后摘链，两者配对保证不丢唤醒。

配置类接口（`set_focus`、`set_global`、浮层、鼠标绑定、定时器、`on_caps`）只能在 `run()`
之前或渲染线程上调用；业务线程经 `post` 间接调用。

---

## 5. 帧调度与渲染管线

### 5.1 调度

主循环每轮：计算 `poll` 超时 → `poll`（stdin、信号管道、唤醒管道）→ 处理信号 → 读输入并
解码、路由 → 执行更新队列 → 执行到期定时器 → Esc 超时、握手超时 → 需要且合帧余量已到则出帧。

`poll` 超时取以下时刻的最小值；一个都没有时无限期阻塞：

- 定时器堆顶（§12.4；已取消的条目先出堆，取消不造成空唤醒）；
- 转义歧义的 40ms 超时（§11.1）；
- 能力握手的 1 秒超时（§6.2）；
- 已失效时的合帧余量（上一帧时刻 + `min_frame`）。

终端尺寸只在渲染线程读（`ioctl` 约 1µs）：SIGWINCH 唤醒时与每帧开头各查一次。尺寸变化时
提升布局纪元、整树补画，并经路由下发 `Kind::resize` 事件。

诊断计数 `frames()`（出帧数）与 `wakeups()`（`poll` 返回次数）用于验证合帧与零唤醒。

### 5.2 单元格网格

`Surface` 是单元格网格而不是行字符串：相邻、重叠控件的输出能正确合成；宽字符占两列、组合
字符占零列在 `Cell` 里显式表达，「第 N 列」是 O(1) 查询；差分能做到行内区间粒度。

- `Cell` 16 字节：4 字节内联字素（≤4 字节的 UTF-8 直接存；更长的簇存 intern 表索引，§7.3）、
  宽度（0 = 宽字符右半占位，1，2）、样式。
- `Style` = 前景色 + 背景色 + 属性位（bold / dim / italic / underline / blink / reverse /
  strike）。`Color` 是默认色、256 色索引或 RGB。
- 制表符在写入时展开为空格，网格里不存 `\t`（否则每次列计算都要回溯）。
- `view(rect)` 得到裁剪视图，控件在自己的局部坐标里绘制，越界写入结构性不可能。

### 5.3 出帧与差分写出

```
业务更新 → post 入队 → 渲染线程执行 → 控件置脏
尺寸变化 → 布局纪元 + 整树补画
                         ↓
       back_ 从 front_ 复制（未失效区域即终端真相）
                         ↓
       root.render(back_)：Container 跳过干净子树
                         ↓
       present：行内区间差分 + SGR 游程 + DEC 2026 包帧 + 光标定位
```

- 重画粒度由两个独立脏标记驱动：`dirty_`（内容变了、尺寸没变 → 只需重画）与
  `layout_dirty_`（可能影响尺寸 → 重新布局，沿树聚合）。
- `present` 逐行比较 back 与 front，只写出变化区间；样式变化才补发 SGR，帧末归零样式；
  终端支持 DEC 2026 时整帧包在同步输出里。真彩色终端写 24 位色，否则量化到 256 色 ——
  主题不需要为低能力终端单独配色。
- 光标来源（焦点控件的 `cursor()`，沿父链累加到屏幕坐标）在帧末定位并显示；没有光标来源时
  隐藏。
- back 与 front 尺寸不一致时（resize、挂起恢复、intern 清表）走全量重写。

---

## 6. L1 终端

### 6.1 进入与还原

`Terminal` 是 RAII：构造即进入界面模式，析构还原。进入顺序：备用屏（1049）→ 关自动换行 →
藏光标 → 括号粘贴（按能力）；stdin 是 tty 时切 raw 模式。还原是严格逆序，只关自己开过的模式
（鼠标、焦点、kitty 键盘 flag、mode 2027、括号粘贴 → 光标 → 自动换行 → 离开备用屏 → termios）。
stdout 不是 tty 时不进入界面模式、不写任何转义。

还原有三条保障路径：析构（正常返回 / 栈展开）；SIGINT/SIGTERM/SIGHUP 经 self-pipe 唤醒
主循环走正常退出；atexit 兜底。`restore()` 幂等，多路径可安全叠加。SIGKILL/SIGSEGV 无法
覆盖，是终端程序的共同边界。

信号处理器只往 self-pipe 写一个字节（`q` = 退出类，`w` = 尺寸变化），不在信号上下文里做
任何别的事。

### 6.2 能力探测与握手

环境变量（`TERM`、`COLORTERM`）给出初始能力。stdin 与 stdout 都是 tty 时，`run()` 开始发出
一组查询并打开解码器的应答窗口（§11.2），**不阻塞首帧**：

| 查询 | 支持的应答 | 用途 |
| --- | --- | --- |
| `\e[?2026$p` | `\e[?2026;{1,2}$y` | 同步输出 |
| `\e[?2027$p` | `\e[?2027;{1,2}$y` | 字素簇宽度模式（§7.2） |
| `\e[?u` | `\e[?{flags}u` | 推入 kitty 键盘 flag 1（§11.2） |
| `\e]11;?\e\\` | `\e]11;rgb:RRRR/GGGG/BBBB` | 背景色 → 明暗主题（§9.2） |
| `\e[c`（DA1，最后发） | `\e[?...c` | 哨兵：收到即结束握手 |

- 终端按顺序应答且都回 DA1，因此 DA1 之前没收到的查询视为不支持；有应答则以应答为准，
  包括显式回「不支持」时覆盖环境变量的乐观猜测。
- 收到 DA1：一次性提交能力，按结果推入 kitty flag 1、开启 `\e[?2027h`。1 秒内没收到 DA1：
  关闭窗口，保持初始值。非交互终端不握手。
- 三种结局都回调一次 `Runtime::on_caps`（§12.5）；应用据背景色选主题。
- 窗口期间的应答由运行时消费，不下发给应用。tmux 内部分查询由 tmux 自己应答，以应答为准，
  不做透传包装。

`Terminal::Caps`：`truecolor`、`synchronized`、`sgr_mouse`、`bracketed_paste`、
`focus_events`、`kitty_keyboard`、`grapheme_width`、`background`（`optional<Color>`）。

### 6.3 挂起与恢复

- `Terminal::suspend()`：按还原栈逆序退出界面模式、还原 termios，记住挂起前开着的鼠标、焦点、
  粘贴、kitty、2027 模式；信号处理器保留。`resume()` 重新进入备用屏与 raw，并恢复这些模式。
  `restore()` 是唯一终态：之后 `suspend` / `resume` 都是空操作。
- `Runtime::run_external(fn)`：suspend → fn（例如 fork/exec `$EDITOR` 并等待）→ resume →
  作废 front、查尺寸、整屏重画。fn 期间 `post` 在队列里累积，恢复后统一执行。
- `Runtime::suspend_process()`：suspend → `kill(0, SIGTSTP)`（与终端 Ctrl+Z 一致，应用拉起的
  子进程一并停住）→ 收到 SIGCONT 后恢复并整屏重画。raw 模式下 Ctrl+Z 以字节 0x1A 到达，
  由应用的全局处理器决定是否调用。
- 挂起期间 SIGINT 不触发退出：外部程序若运行在规范模式，Ctrl+C 会送达整个前台进程组，它属于
  外部程序。只在信号处理器里忽略，不改成 `SIG_IGN`（被忽略的信号跨 exec 继承，外部程序会收不到
  Ctrl+C）；SIGTERM/SIGHUP 照常退出。

### 6.4 上报模式与剪贴板

- `set_mouse(true)`：1002（按键事件跟踪：按下、释放、按住时的移动）+ 1006（SGR 编码）。1000
  不报按住时的移动，拖拽选择收不到；1003 连悬停也报，事件量大且无用。开启后终端不再做原生
  选择（多数终端按住 Shift 仍可原生选择），应用内选择由 §10.10 接管。默认关闭，由应用决定。
- `set_focus_events`：1004。括号粘贴由构造按能力开启，不提供运行时开关。
- `set_clipboard(text)`：写 OSC 52 `\e]52;c;<base64>\a`。上限 1 MiB 原文，超出按 UTF-8 边界
  截断并返回 false，由应用提示。tmux 需要 `set-clipboard on`，属使用方配置。

---

## 7. L2 字素与宽度

### 7.1 字素簇

`next_grapheme` 按 UAX #29 聚合字素簇，有意不实现两条规则：GB9b（Prepend ×）与 GB9c
（Indic 辅音连缀）。其余规则与 Unicode 官方 `GraphemeBreakTest.txt` 一致（测试按这两条规则
涉及的码点清单排除对应用例）。输入可以在任意字节处被截断：末尾不完整的 UTF-8 序列留给调用方
续接。

### 7.2 宽度

宽度表与字素属性表由 Unicode 17.0 数据文件（`EastAsianWidth.txt`、`emoji-data.txt`、
`GraphemeBreakProperty.txt`、`DerivedGeneralCategory.txt`）生成，写在 `grapheme.cpp` 的
标记区段里，注释记录版本与生成命令；生成脚本不入库，该区段不手改。

- EastAsianWidth W/F 与 `Emoji_Presentation=Yes` 宽 2；后随 VS16（U+FE0F）的簇宽 2；
  组合字符、零宽字符宽 0。
- 握手确认支持 mode 2027 时开启 `\e[?2027h`，终端与框架使用同一套字素簇规则。不支持的终端
  按码点宽度累加，ZWJ 序列等可能与框架计算不一致（§16）。

### 7.3 intern 表

超过 4 字节的字素（emoji ZWJ 序列等）存进进程级 intern 表，`Cell` 里只存索引。表是渲染线程
专用、无锁的。项数超过 4096 时置溢出标志；运行时在写出一帧后清表、作废双缓冲并整树补画，
下一帧全量重写后旧索引自然消失。清表只能在帧间做 —— 缓冲区里已有的单元格持有旧索引。

---

## 8. L3 布局与层栈

### 8.1 容器布局

`Widget` 基类只有布局与绘制：`measure`、`layout`、`render`、`cursor`、失效标记、父子关系、
屏幕坐标（`screen_origin` 沿父链累加）、`invalidate_rect`、`hit_test`。L3 不预留任何事件、
焦点 API。

`Container` 沿主轴（vertical / horizontal）排布子项，副轴占满。约束 `Constraint{sizing,
value, min, max}`：

1. `fixed`：直接占用 value；
2. `content`：调 `measure(剩余空间)`，夹到 [min, max]，可为 0（不占位）；
3. `flex`：剩余空间按权重分摊（floor + 余量按声明顺序补 1）；
4. 总需求超出可用空间时按声明顺序逆序压缩，直到 min；Σmin 仍超出时按父边界硬截断。

渲染时跳过干净子项；布局收缩后尾部腾出的区域（gap）清空一次。

### 8.2 层栈与浮层

根控件是 `LayerStack`：一个基础层 + 按 z 序排列的浮层。它的矩形就是屏幕，因此浮层矩形与其
子孙的 `screen_origin()` 直接是屏幕坐标。

- `push(overlay, placement, point)`：尺寸取 `overlay.measure(屏幕尺寸)` 并夹到屏幕内；
  `move` 重新摆放；`remove` 摘除并归还所有权。
- `Placement`：`center`（对话框、命令面板）、`top_right`（toast）、`above_point`（贴点上方，
  空间不足翻到下方 —— 补全弹窗贴光标）、`at_point`。
- 浮层不透明，不做 alpha 混合；背景变暗效果由应用自己画一个全屏遮罩浮层。

### 8.3 损伤传播

保留「干净控件跳过 + back 复制 front」的优化，用损伤矩形保证正确性。每帧：

1. `damage` = 本帧被移除、移动、改尺寸的浮层的**旧**矩形。损伤区域直接擦成空白（浮层下方
   可能没有控件，只让相交控件失效会留下旧像素），并让基础层中与之相交的控件失效（`Container`
   递归，深层控件也会补画）。
2. 渲染基础层；`Container` 记录本帧实际重画的子项屏幕矩形，汇总为 `painted`。
3. 按 z 序遍历浮层：自身失效，或与 `painted ∪ damage` 相交 → 整层重画，并把其矩形并入
   `painted`，更上层的浮层随之重画。

正确性由「增量 == 全量」保证：任意打开、移动、关闭、失效、改尺寸序列之后，增量路径得到的
back 与整树失效后全量渲染的结果逐格相同。

### 8.4 命中

`hit_test(point)` 返回包含该点的最深控件：容器逆序遍历子项（后声明的在上面），层栈先逆序
遍历浮层再落回基础层。`LayerStack::hit` 是鼠标分发的入口（§12.3）。

---

## 9. L4 控件与主题令牌

### 9.1 控件

L4 只提供程序化的内容与编辑模型，不处理事件；按键翻译在 L6（`InputBoxHandler`），界面语义
在应用层。

| 控件 | 内容 |
| --- | --- |
| `Text` | 多行静态文本，单一样式；自然宽度取最长行 |
| `Activity` | 转圈符号 + 当前动作；文本为空时占 0 行。`tick()` 推进一帧，节奏由应用用 `every`（§12.4）驱动 |
| `Notice` | 单行通知，严重级别映射 info / warning / error 令牌；文本为空时占 0 行 |
| `InputBox` | 边框 + 多行编辑模型（插入、退格、删除、移动、行首行尾）+ 光标；滚动在 `render` 时推进 |

每个控件有 `set_theme(const ThemeTokens&)`，只保存引用（主题对象必须比控件活得久）。

### 9.2 语义主题令牌

所有渲染器与控件只引用 `ThemeTokens`，不写死颜色：

```cpp
struct ThemeTokens {
    Style text, text_muted, primary, accent;
    Style border, border_active, background, background_panel, background_element;
    Style success, warning, error, info;
    Style diff_added, diff_removed, diff_context, diff_hunk;
    Style syntax_keyword, syntax_string, syntax_comment, syntax_number,
          syntax_function, syntax_type;
    Style markdown_heading, markdown_code, markdown_link, markdown_quote;
    Style selection;
    uint32_t epoch = 0;
};
```

- **epoch 是失效契约**：任何令牌变化后必须递增 `epoch`，Document 的物化缓存（缓存键含 epoch）
  据此整体失效、重排使用新样式。只改令牌不递增 epoch，缓存不失效。
- `background*` 令牌是底色：颜色在 `bg`，`fg` 保持默认。在面板上写字的样式需补上面板底色，
  否则会覆盖掉铺好的底色。
- `selection`：设了 fg / bg 就覆盖，反色属性取翻转（原样式已是反色时仍可见），其余属性取并集。
- 内置 `dark_theme()` / `light_theme()`（256 色）。`default_theme(background)` 按背景色的相对
  亮度（阈值 0.5）选择，取不到背景色时取 dark。主题文件的格式与加载属于应用层（§13）。

---

## 10. L5 文档层

`Document` 是大内容滚动区的数据模型，`Scrollback` 是它的门面控件。渲染时按视口宽度折行、
只物化可见窗口；内容变更由 revision 驱动重画，不需要手动失效。

### 10.1 块模型

逻辑块 `Block` 是渲染与裁剪的单位：

- 逻辑内容：`id`（单调递增、永不复用，锚点引用它）、`kind`、`source`（未折行、未上色的原文）、
  `meta`（附属信息，例如代码块的语言）、`group`、`depth`、`open`（仍在增长）、`collapsed` /
  `collapsed_rows`（折叠后最多显示的行数）、`margin_top`（块前空行数，§10.5）。
- 派生缓存（Document 维护）：`cache_key`（宽度 + 折叠状态 + 主题纪元）、`row_count`、
  `stable_rows` / `stable_bytes`（§10.3）、`rows`（物化行）、`rows_valid` / `rows_bytes`
  （物化的有效前缀）。

`BlockKind`：`text`、`code`、`diff`、`output`、`markdown`、`table`。

物化行 `Line` 由若干 `Span`（同样式的一段文本，制表符已展开）组成，另有显示宽度、行首在
source 中的字节偏移（锚点解析用）、语法渲染器的跨行词法状态。`Span::src` 记录该段显示文本的
源偏移（装饰为 `k_no_src`），供选择把屏幕坐标换算成逻辑位置（§10.10）。

### 10.2 计数与物化

每帧 `begin_frame(width, epoch)` 先做**计数级**折行：对所有块只数行、零分配，结果进入行数
前缀和（块行数 = 生效的上边距 + 内容行数）。宽度或主题纪元变化时整体重数一次（仍零分配），
物化缓存整体作废；否则只对脏块增量计数，前缀和从最靠前的脏块索引开始重建，代价
O(块数 − 该索引)。

**物化**只发生在可见窗口（`materialize_range`），并驱逐可见窗口 ± 一屏以外块的物化结果；
open 块不驱逐 —— 它的 rows 就是增量折行的锚点。

**等价性**：任何渲染器都必须满足 `measure(s, 0, w).rows == render 产出的行数`。默认渲染器的
计数与物化走同一条扫描路径（`wrap_next_row`），自定义渲染器按行推进时也必须复用它。

### 10.3 增量折行与断点

`wrap_next_row` 扫描一行：宽度到顶时回退到最后一个断点，没有断点则按字素硬断（长单词、URL
不丢内容）；换行符结束本行。断点候选：

- 空格之后；
- 宽字符（CJK）的前后 —— 中文句子没有空格，只认空格会在中英混排时退回到很靠前的英文空格，
  在纯中文长句里按行宽硬断。

断点受简化禁则约束：候选位置之后不能是闭合标点（`，。、；：！？）」』】》…` 及 ASCII
`,.;:!?)]}%` 等），之前不能是开启标点（`（「『【《“` 及 `([{`）。

**已定行**：除最后一行外，断点由已扫过的字符唯一决定。计数结果带出 `stable_rows` /
`stable_bytes`（已定前缀的边界），流式追加后只从 `stable_bytes` 重扫最后一行，代价 O(最后一行)。
以 `\r` 结尾、或末尾是不完整 UTF-8 序列的行不算已定（补齐后可能改变断点）。渲染器做不到这一点
时返回 `stable_rows = 0`，退化为每帧整块计数与物化。

### 10.4 裁剪与定位

- `trim_blocks(keep)` / `trim_rows(keep)`：头部裁剪，O(1)/块 —— 只弹出块并抬高全局行偏移
  `base_rows_`，前缀和里的绝对行号不平移。
- 块 id 单调递增，`blocks_` 按 id 有序，按 id 定位是二分。
- `row_of(block, byte)`：锚点 → 行号；`location_of(row)`：行号 → (块 id, 行首字节)；两者都会
  物化所在块。

### 10.5 锚点与块间距

**锚点**（`Anchor{block_id, byte_in_block, pinned_to_bottom}`）由 Document 持有，`Scrollback`
读取与重定位。贴底是独立状态：新内容到达时贴底则跟随，不贴底则锚点不动。位置用块内字节偏移
而不是行号，改变宽度后视口仍停在同一段内容上；块被裁剪或内容缩水导致夹取时，锚点跟随实际
视口顶行。

**块间距**：`margin_top` 由产生块的一方设置（只有它知道语义），Document 计入前缀和并在块前
补空行，渲染器不感知。没有内容行的块边距折叠为 0，流式新开的空块不会先冒出一行空白。边距行
没有内容可锚：`location_of` 把它映射到下方块首；`Scrollback` 向上滚动落在边距行时越过边距、
锚到上一块末行，视口顶行因此从不停在边距行上，上下滚动都不会卡住。

### 10.6 变更 API

- `append_block` / `open_block`：新块立即计数，不到可见之前不物化。多个块可以同时 open。
- `append(id, chunk)`：只 `source += chunk` 并置脏，O(chunk)，不折行、不分配、零 I/O。
- `replace(id, source)`：任意块整体替换，重置该块缓存；锚点在该块时字节偏移夹到新长度。
- `set_meta(id, meta)`：只失效物化缓存，计数不受影响。
- `erase_from(id)`：删除 id 及其后所有块（撤销），前缀和截断；锚点块被删除时移到删除点之前
  最后一块的末尾，文档已空则贴底。
- `close_block`、`set_collapsed`、`clear`、`set_renderer`（替换某类块的渲染器，已有块整体置脏）。

### 10.7 块渲染器

`BlockRenderer` 有两个接口：`measure(source, from, width)`（只数行、零分配，返回已定前缀）与
`render(block, width, theme, from, valid, out)`（从 `from` 起物化，覆盖写入 `out[valid...]`）。
默认注册表：

| 块类型 | 渲染器 | 说明 |
| --- | --- | --- |
| `text` / `output` | `TextRenderer` | 每行一段，样式取 `text` / `text_muted` 令牌 |
| `diff` | `DiffRenderer` | `+` / `-` / `@` 开头的行分别用 diff_added / diff_removed / diff_hunk |
| `markdown` | `MarkdownRenderer` | 见下 |
| `table` | `TableRenderer` | 见下 |
| `code` | `SyntaxRenderer` | §10.9 |

**Markdown**：块级前缀用令牌着色（标题保留 `#`，列表符号换成 `•`，引用换成 `│`，分隔线画成
整行 `─`）；行内标记（`**` `*` `_` 反引号、`[文本](地址)`、反斜杠转义）**隐藏**，只显示带样式的
正文，链接地址不显示。排版以「段」为单位：一个块级起始行加上其后的续行（普通文本并入段落、
列表项、引用；同层引用行合并），强调可跨软换行与折行配对。显示文本比原文短，计数与物化共用
同一个排版函数、按显示文本折行，行数必然一致。标记配对遵循简化的 CommonMark 规则：开标记后
不能是空白，闭标记前不能是空白，`_` 不在词内生效，配不上对的按字面输出。整块每帧重排
（`stable_rows = 0`），代价受段落长度约束。

**表格**：列宽依赖全部行，每次变化整块重排。单元格与段落走同一个行内解析器，列宽按隐藏标记
后的显示宽度计算；总宽放不下时按比例收缩，单元格超宽截断加 `…`。首个内容行是表头（primary），
分隔行画成 `├─┼─┤`。

### 10.8 流式 Markdown

增量折行依赖「已定行不会因追加而改变」，Markdown 不满足（未闭合的 `**` 会改变前文样式，
围栏打开后其后全部内容换成代码，表格列宽取决于所有行）。`MarkdownStream`（渲染线程使用）
因此把一条流式消息按块级结构切成多个 Document 块，**只有最后一个块在增长**：

| 块级结构 | BlockKind | 块何时关闭 |
| --- | --- | --- |
| 段落、标题、列表、引用 | `markdown` | 空行，或下一行开启其他块级结构（列表与引用的续行不算） |
| 围栏代码（``` / ~~~，info 串存入 `meta`，围栏行不进 source） | `code` | 匹配的闭合围栏 |
| 表格（表头 + 分隔行，可打断段落） | `table` | 空行或不再以 `\|` 开头的行 |
| 分隔线 | `markdown` | 立即 |

- 结构判定全部发生在完整行上，结果与喂入分块无关（可以切在多字节字符中间）。
- 未完成的行先追加到当前块立即显示；整行到达后判定为新结构时用 `replace` 回退该行（整块替换，
  O(当前块)，与 markdown / 表格块每帧的整块重排同阶）。可能是围栏或表格开头的行先攒着，
  整行到达再显示。
- 源码里的空行不进块；消息内除第一块外每块 `margin_top = 1`（段落间距），第一块取构造参数
  `first_margin`（消息之间的间距归应用）。
- `finish()` 把未完成的行按现状保留、关闭最后一块，状态复位，同一实例可以开始下一条消息。

### 10.9 语法高亮

`SyntaxRenderer` 是内置轻量词法高亮器：关键字、字符串、注释、数字、函数名、类型名；语言取自
`meta` 的首个词，覆盖 C/C++、Python、JavaScript/TypeScript、JSON、Bash、Go、Rust，未识别的
按纯文本。不引入 tree-sitter。

- 按整条逻辑行做词法分析，再按折行的字节范围切片 —— 行注释、字符串、「标识符后的 `(` 是函数」
  这类判定不会被折行打断。
- 跨逻辑行的状态（块注释含嵌套深度、Python 三引号、JS 模板串、Go/Rust 原始串、Bash 引号）写进
  `Line::lex`。增量续扫回退到最后一条逻辑行的行首，从上一逻辑行的状态继续：代码块保持增量，
  代价 O(最后一行)。
- 视觉区分：每行左侧一道竖条（`border_active`），整行铺 `background_element` 底色；竖条占 2 列，
  计数与物化都按「宽度 − 2」折行。

### 10.10 选择与复制

- **选择模型**：`Selection{Location anchor, Location head}`，两端都指向字素起点、闭区间。用逻辑
  位置（块 id + 字节偏移）表示，改变宽度后选区仍覆盖同一段内容。`Location` 按 (块 id, 字节)
  比较即文档顺序。
- **坐标换算**：`Document::location_at(row, col)` 借 `Span::src` 把屏幕格换算成源字节。装饰
  （列表符号、表格边框、代码块竖条）归属于其后的内容；行尾之后在硬换行、块尾处取行尾（含换行），
  在软折行处取行内最后一个字素；边距行取块首。
- **复制内容**：`text_between(a, b)` 返回两位置之间的**源文本**（Markdown 原文），软折行不插入
  换行；跨块时补齐块间换行，有上边距的块前空一行。代码块的围栏行不在 source 里，复制内容不含围栏。
- **扩展**：`word_around`（字母数字、`_` 与非 ASCII 字符的连续段）、`line_around`（逻辑行，
  不含行尾换行）。
- **绘制**：`Scrollback` 持有选区（`hit` / `select` / `clear_selection` / `selected_text`），
  绘制时逐字素判定并叠加 `selection` 令牌，不写进物化缓存；选区变化只失效滚动区视图。
- 交互由 L7 的 `ScrollbackMouse` 完成（§12.7）。

---

## 11. L6 输入

### 11.1 解码器

`Decoder` 是纯字节状态机：不知道任何控件，不持有时钟。喂进任意分块的字节流，吐出事件；内部
缓冲把跨读取边界的多字节 UTF-8 与截断的转义序列拼回来。同一次 `feed` 内连续的可打印字符合并为
一个 text 事件。无法识别的序列完整读到终止符再整体丢弃 —— 残留字节混进输入框是终端程序最常见
的 bug，这里在结构上排除。

- 事件：`text`、`key`（命名键，或 Ctrl/Alt + 可打印字符）、`mouse`（SGR 1006；按键、屏幕行列、
  相对命中控件的坐标、按下/移动、`outside`）、`paste`（括号粘贴，结束标记增量查找）、`resize`
  （由 L7 合成）、`focus`（1004）、`reply`（§11.2）。
- 修饰键组合与 xterm 的 modifier 参数一致，另有 kitty 的 super 位；功能键覆盖 F1–F12、方向、
  Home/End/PageUp/PageDown/Insert/Delete。
- **Esc 歧义**：孤立 ESC、`\e[` / `\eO` 引导符、残缺的 CSI 停在歧义窗口，`pending_escape()`
  为真时调用方给 `poll` 加 40ms 超时；超时仍无后续字节即 `flush_escape()`：孤立 ESC 串每个产出
  一个 Esc 键，`\e[` / `\eO` 产出 Alt-[ / Alt-O，带参数的残缺序列整体丢弃。

### 11.2 终端应答与 kitty 键盘协议

`\e]` 同时是 OSC 引导符与用户按下的 Alt-]，不能无条件按 OSC 解析。解码器因此有**应答窗口**
（`set_reply_window`，由 L7 在握手期间打开）：

- 窗口打开时：`\e]` `\eP` `\e_` `\e^` `\eX` 开始终端字符串，读到 BEL（仅 OSC）或 ST 为止，
  产出一个 `reply` 事件（`reply_type` 区分 csi / osc / dcs / apc）；带私有标记 `?` `>` `=`
  的 CSI（DA1、DECRQM、kitty 查询应答）也产出 `reply`。字符串上限 1 MiB，超出部分丢弃但仍读到
  终止符。窗口内未读完的应答不参与 Esc 超时；窗口关闭时仍未终止的整体丢弃。
- 窗口关闭时：`\e]` 是 Alt-]，私有 CSI 整体丢弃。

kitty 键盘协议只用 flag 1（消歧转义码）：握手确认后推入，还原与挂起时弹出。解码
`CSI code[:alternate];mods[:event] u` 与带修饰的 `~` / 字母终止键；13/9/127/27 映射为
enter/tab/backspace/escape，其余可打印码点有修饰时产出 key + text、无修饰时产出 text；带
shift 且有 alternate 时取 alternate 作为上屏字符；release 事件丢弃。不请求按键释放/重复事件。

### 11.3 路由

`EventRouter` 的下沉顺序固定：**模态处理器栈（栈顶优先）→ 焦点处理器 → 全局处理器**，第一个
返回 true 的消费事件。「某个键在某种状态下归谁」由栈的顺序回答，不需要条件判断。处理器可以在
`on_event` 里压栈、弹栈：期间压栈的不参与本次下发，期间弹出的立即不再被调用（弹出后马上销毁是
安全的）；下发期间弹栈只把槽位置空，最外层下发结束时统一压实，零分配。

鼠标事件不走这条链，由 L7 按坐标分发（§12.3）。

`InputBoxHandler` 是 `InputBox` 的按键翻译：文本与粘贴 → 插入（粘贴里的换行只分行、不提交）；
退格、删除、方向、Home/End、Tab → 对应编辑操作。Enter、Escape、带修饰的方向键一律不消费 ——
提交、补全、词间移动是应用策略。

---

## 12. L7 运行时

### 12.1 接口

| 接口 | 线程 | 作用 |
| --- | --- | --- |
| `post(fn)` | 任意 | 更新队列（§4） |
| `quit()` | 任意 | 请求退出 |
| `run()` | 调用者成为渲染线程 | 主循环，直到 quit、退出类信号或 stdin 关闭 |
| `set_focus(handler, cursor_source)` | 渲染 | 焦点路由目标与光标来源 |
| `set_global(handler)`、`push_modal` / `pop_modal` | 渲染 | 路由链（§11.3） |
| `open_overlay` / `close_overlay` | 渲染 | 浮层（§12.2） |
| `bind_mouse` / `unbind_mouse` | 渲染 | 鼠标分发（§12.3） |
| `after` / `every` / `cancel` | 渲染 | 定时器（§12.4） |
| `on_caps(fn)` | 渲染 | 能力确定回调（§12.5） |
| `run_external(fn)` / `suspend_process()` | 渲染 | 挂起与恢复（§6.3） |
| `set_clipboard(text)` | 渲染 | OSC 52（§6.4），调用即写出，不等下一帧 |
| `frames()` / `wakeups()` | 任意 | 诊断计数 |

### 12.2 浮层与焦点

`open_overlay(widget, placement, point, modal, cursor_source)` 把浮层压进层栈（根控件必须是
`LayerStack`），可选接管输入（`modal` 压入模态栈）与光标来源；`close_overlay` 弹出模态、恢复
打开前的光标来源，并解除浮层子树内控件的鼠标绑定与捕获。关闭顺序不是后进先出时：模态栈按空槽
语义处理；上层浮层若把先关浮层的光标来源记作恢复点，恢复点改接到先关浮层自己的恢复点，不会
恢复成已销毁的控件。

### 12.3 鼠标分发

控件与鼠标处理器的对应关系登记在 L7（`bind_mouse`），L3/L4 不感知事件。分发顺序：

1. **捕获**：非滚轮按下被命中链上的处理器消费后，直到释放前，该按钮的移动与释放都直接交给它，
   不论指针是否移出（拖拽选择）。
2. **模态**：最上层带模态处理器的浮层不包含指针时，事件交给该模态处理器（`outside = true`）
   并吞掉，不穿透到下层；是否关闭由处理器决定。
3. **命中链**：`LayerStack::hit` 找到最深控件，沿父链向上，第一个绑定了处理器且消费的为止。
4. **全局**处理器。

事件的 `x` / `y` 在投递前改写为相对接收控件左上角（捕获期间相对捕获控件）。`unbind_mouse` 与
`close_overlay` 同时清除指向被解绑控件的捕获；控件销毁前必须解绑（浮层内的由 `close_overlay`
代为解除）。

### 12.4 定时器

```cpp
TimerId after(std::chrono::milliseconds delay, std::function<void()> fn);
TimerId every(std::chrono::milliseconds period, std::function<bool()> fn); // 返回 false 即停止
void cancel(TimerId id);
```

- 到期时刻存最小堆，同时到期的按创建顺序执行；取消只从登记表删除，堆里的旧条目弹出时丢弃
  （惰性删除），积压过多时整堆重建。
- 每轮先摘出全部已到期条目再执行：回调里新建的 `after(0)` 留到下一轮，不会在同一轮里无限续命。
  回调执行前函数移出登记表，回调可以安全地取消自己或新建定时器。
- `every` 按周期对齐推进；落后（慢帧）时从现在起算，不补发积压的周期。
- 动画用 `every`：启动动画的代码显式调用，回调返回 false 即停止。没有定时器时主循环无限期阻塞。

### 12.5 能力回调

握手是异步的，背景色在首帧之后才到。`on_caps(fn)` 在能力确定时回调一次：DA1 提交、1 秒超时、
或不握手（`run()` 开始即回调）。能力确定后才注册的，注册时立即回调。回调里通常会换主题，随后
照常检查失效、合帧出帧。

### 12.6 快捷键与命令层

```cpp
struct Command {
    std::string id, title, category;
    std::function<void()> run;
    std::function<bool()> enabled; // 空 = 始终可用
};
class Keymap : public EventHandler {
    void add(Command c);                         // 重复 id 覆盖
    bool bind(std::string_view keys, std::string_view command_id);
    void set_leader(std::string_view key, std::chrono::milliseconds timeout);
    const std::vector<Command>& commands() const noexcept; // 命令面板数据源
};
```

- **绑定串**：空格分隔的按键序列，每个按键是 `修饰+键`（`ctrl` / `alt` / `shift` / `super`；
  命名键 `enter` `tab` `escape` `space` `backspace` 方向 `home` `end` `pageup` `pagedown`
  `insert` `delete` `f1`–`f12`，或单个可打印 ASCII 字符），`<leader>` 展开为 `set_leader` 的按键。
  非法串或不存在的命令 `bind` 返回 false；同一序列后绑定覆盖先绑定。
- **分派**：`Keymap` 作为全局处理器安装。单键绑定在事件到达时直接执行；多键序列（含 leader
  开头）在首键按下时把自身压入模态栈并启动超时定时器，后续按键不会先被输入框当作文本消费；
  匹配成功、不匹配或超时后弹出，不匹配的按键照常沿栈下沉。命中序列同时是更长序列的前缀时立即
  执行。`enabled` 返回 false 时按键被消费但不执行。
- **上下文归属**仍由栈顺序决定：对话框消费了 Escape，它就不到达 `Keymap`。
- **按键归一化**：Shift 与字母的组合一律表示为「小写字母 + shift」—— 终端把 Shift+A 报成无修饰
  的文本 `"A"`，因此 `"shift+a"` 与 `"A"` 等价；Shift 与符号的组合按终端报出的字符匹配（写 `"?"`
  而不是 `"shift+/"`）。
- 绑定表的来源（配置文件）属于应用层；框架只提供解析与分派。
- 生命周期：`Keymap` 必须先于 `Runtime` 销毁（析构要取消定时器、弹出自压的模态）。

### 12.7 滚动区鼠标

`ScrollbackMouse` 把鼠标翻译成 `Scrollback` 的选择调用，经 `bind_mouse` 绑到滚动区（需要应用
开启鼠标上报）：

- 左键拖拽选择（依赖捕获，拖出控件或视口也归它；拖出上方选到行首、下方选到行尾）；单击不拖拽
  清除选区。
- 同一格 400ms 内连击：双击选词、三击选逻辑行（由 `after` 判定连击窗口）。
- 释放时把选区源文本写入剪贴板（`copy_on_release = false` 关闭）。
- 滚轮滚动 3 行。
- 必须先于 `Runtime` 与 `Scrollback` 销毁。

---

## 13. 应用层约定

应用层代码在 `ui` 模块（`src/public/ui`、`src/private/ui`，静态库 `dagent_ui`，依赖
`dagent_tui`）。第三方头文件放在 `src/public/lib/`（当前是 nlohmann/json v3.12.0）。

**主题文件**：`ui::load_theme(path)` 读取 JSON 主题，返回 `ThemeSet{name, dark, light}`；
`ThemeSet::pick(background)` 按框架的 `default_theme` 规则选明暗。默认主题是
`config/themes/dagent.json`：

```json
{
  "name": "dagent",
  "defs":  { "blue": "#7aa2f7", ... },
  "dark":  { "primary": "blue", "error": { "fg": "red", "attrs": ["bold"] }, ... },
  "light": { ... }
}
```

样式可以是 `{"fg", "bg", "attrs"}` 完整形式，也可以直接写一个颜色（只设前景色）。颜色取值：
`#rrggbb`、0–255 的 256 色索引、`defs` 里的名字、`"default"`。令牌名与 `ThemeTokens` 字段同名；
文件里没写的令牌沿用内置取值。应用在 `on_caps` 回调里选明暗，设置前递增 epoch。

---

## 14. 测试

`test/tui`（Boost.Test，可执行文件 `tui_tests`）只覆盖最近的里程碑，用真实线程、真实管道 /
pty 与真实数据，不做烟雾测试：

| 文件 | 覆盖 |
| --- | --- |
| `document_test.cpp` | 选区跨软折行复制源文本；改变宽度后选区与反色不变；主题 epoch 失效；明暗选择；中文折行与禁则、表格行内样式、代码块竖条 |
| `app_test.cpp` | 定时器顺序与取消；静置零唤醒零帧；pty 下拖拽/双击的 OSC 52 负载；快捷键解析与分派、leader 超时回落；握手开启 2027；挂起恢复与整屏重画（含挂起期间 SIGINT）；`on_caps` 选主题 |
| `grapheme_test.cpp` | Unicode 官方 `GraphemeBreakTest.txt`（`test/tui/data/`，按 GB9b/GB9c 清单排除）；emoji 与 VS16 宽度；intern 表上限与清表重画一致 |

pty 用例由父进程在主端扮演终端：读取查询、写回应答、注入按键与 SGR 鼠标序列、断言输出。

**必须在真实终端目视确认**（断言替代不了）：握手在 kitty / alacritty / wezterm / ghostty /
gnome-terminal / VS Code 内置终端 / tmux 中的实际应答与降级；tmux 下 OSC 52；Ctrl+Z 与 `$EDITOR`
的挂起恢复（孤儿进程组中 SIGTSTP 会被内核丢弃，测试进程里无法验证）；emoji 与 mode 2027 在各终端
的宽度表现；浮层打开关闭时有无闪烁。`temp/agent_demo/`（不入库）是一个脚本驱动的完整 Agent
界面，用于这类目视确认。

---

## 15. 明确不做

- 约束求解布局（flexbox / grid 全集）。fixed / content / flex + 层栈浮层覆盖全部界面需要。
- 单元格以下的差分。
- 通用控件库（按钮、表格、下拉的通用实现）。框架提供原语，界面控件属于应用层。
- 非 ANSI 终端、terminfo。
- 双向文本（BiDi）。
- 窗口管理、可拖拽的分割线（支持横向分栏与浮层）。
- 透明度与 alpha 混合（浮层不透明，遮罩用全屏浮层实现）。
- 图片协议（kitty graphics / sixel）。
- tree-sitter 与任何外部语法库。
- OSC 8 超链接（`Cell` 没有存放链接 id 的空间，收益不足以改变 16 字节布局）。
- 按键释放/重复事件（kitty 协议只用 flag 1）。

---

## 16. 已知缺口

- **GB9b / GB9c 未实现**：Prepend 字符与 Indic 辅音连缀按普通断点处理，相关文字的字素簇与
  终端可能不一致。
- **不支持 mode 2027 的终端**：ZWJ 序列、VS16 等按码点宽度累加，宽度可能与框架不一致，表现为
  行尾错位。
- **intern 表极端情况**：当前屏幕本身需要的超长字素超过 4096 个时，每帧都会溢出清表、整屏重画。
- **Markdown 与表格块整块重排**：段落、表格越长，流式时每帧代价越大（受单块长度约束，不随消息
  总长增长）。
- **复制代码块不含围栏**：围栏行不在块的 source 里。
- **折行禁则是简化版**：只列常用中日文与 ASCII 标点，不实现完整的 UAX #14。
- **拖拽选择不自动滚动**：拖出视口时选区夹到可见的首/末行，需配合滚轮扩展。
- **相对亮度未做 sRGB 线性化**：明暗选择按加权和近似，接近阈值的背景色可能选错。
- **tmux**：不做查询透传包装，能力以 tmux 自己的应答为准。

---

## 参考

- [OpenTUI](https://github.com/anomalyco/opentui)、[渲染管线](https://opentui.com/docs/core-concepts/rendering-pipeline/)、[交互、焦点与选择](https://opentui.com/docs/core-concepts/interaction/)
- [OpenCode TUI 文档](https://opencode.ai/docs/tui/)
- [kitty 键盘协议](https://sw.kovidgoyal.net/kitty/keyboard-protocol/)
- [UAX #29 文本分段](https://www.unicode.org/reports/tr29/)、[UAX #14 断行](https://www.unicode.org/reports/tr14/)
