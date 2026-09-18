# 终端 UI 框架：最终更新（对标 OpenCode）

本文是 TUI 框架的最后一次结构更新，也是这个框架唯一的设计文档（原先的
`00-tui-framework.md` 已删除，其内容并入本文 §零与 §四）。L1–L7 内核已经实现；本文补齐
做出 OpenCode 级交互所需、但内核里还没有的**框架原语**。完成 M1–M6 之后，TUI 框架冻结，
后续只在其上构建应用层（会话视图、输入框、对话框、侧栏等具体界面，不在本文范围）。

基线：`7c7d61c`（L6）+ 未提交的 L7 修复。

---

## 零、当前实现（L1–L7）

### 0.1 分层

依赖只向下，每层的边界就是它**不做**什么。

| 层 | 职责 | 文件 |
| --- | --- | --- |
| L1 | 终端抽象：进入/退出界面模式、能力探测、尺寸报告、字节写出口。不做任何绘制决策 | `terminal.hpp` |
| L2 | 单元格网格 + 双缓冲 + 损伤差分；UTF-8 解码、字素簇聚合、码点显示宽度 | `surface.hpp`、`grapheme.hpp` |
| L3 | 布局（fixed / content / flex）+ `Widget` 基类。不预留任何事件/焦点 API | `layout.hpp` |
| L4 | 具体视图（`Text`、`Activity`、`Notice`、`InputBox`）。只提供程序化的内容与编辑模型 | `widget.hpp` |
| L5 | 文档层：块模型、增量折行、锚点、裁剪、`BlockRenderer` 注册表 | `document.hpp` |
| L6 | 输入层：字节解码 → 事件 → 焦点链路由 | `input.hpp` |
| L7 | 运行时：更新通道、合帧调度、信号、光标定位、差分写出 | `app.hpp` |

### 0.2 三条贯穿全局的约束

**① 业务线程永不被界面阻塞。** 这是整个线程模型的出发点，§3.1 就是为了兑现它而重做的。
控件树与 Document 只属于渲染线程，框架里没有保护它们的锁；业务线程与渲染线程之间只有
一个更新队列。

**② 静止界面零输出、零唤醒。** 帧节奏由唤醒驱动而不是固定频率轮询：按键与业务变更立即
出帧，最小帧间隔（默认 16ms）只用来合并突发。只有控件树真的失效（或尺寸/焦点变化）才
出帧；没有动画、没有输入时 `poll` 无限期阻塞。

**③ source 是唯一真相，屏幕行是派生缓存。** L5 的块模型由此得到：改变宽度能重折、
折叠/展开能切换形态、裁剪按块整块进行；计数（对所有块、零分配）与物化（只对可见块）
分离；流式追加只做 `source += chunk`，折行在渲染时从 `stable_bytes` 增量进行，代价是
O(最后一行) 而不是 O(全文)。滚动位置用 `Anchor`（块 id + 块内行号）而不是「距底部 N 行」，
重折、裁剪、追加都不会让用户正在看的内容跳走。

### 0.3 渲染管线

```
尺寸变化 → 布局纪元 + 整树补画
业务更新 → post 入队 → 渲染线程执行 → 控件置脏
                                    ↓
          back_ 从 front_ 复制（未失效区域即终端真相）
                                    ↓
          root.render(back_)：Container 跳过干净子树
                                    ↓
          present：行内区间差分 + SGR 游程 + DEC 2026 包帧
```

单元格网格而不是行字符串，是因为：相邻/重叠 widget 的输出可以正确合成；样式跨行由差分器
统一补发 SGR；宽字符占两列、组合字符占零列在 `Cell` 里显式表达，「第 N 列」是 O(1) 查询；
差分能做到行内区间粒度而不是整行粒度。

重画粒度由两个独立脏标记驱动：`dirty_`（内容变了但尺寸没变 → 只需重画）与 `layout_dirty_`
（可能影响尺寸 → 重新布局，沿树聚合）。

### 0.4 输入与还原

解码与语义严格分离：`Decoder` 是纯字节状态机，不知道任何控件、不持有时钟（Esc 歧义的
超时判定权交给调用方的 poll 循环）；`EventRouter` 是固定的下沉顺序 —— 处理器栈 → 焦点 →
全局兜底，「某个键在某种状态下归谁」由栈的顺序回答，不需要任何条件判断。解码器不产生
resize：终端尺寸由渲染线程逐帧 ioctl 探测，L7 合成 `Kind::resize` 塞进同一条事件流。

终端还原有三条保障路径：析构（正常返回 / 栈展开）、SIGINT/SIGTERM/SIGHUP 经 self-pipe
唤醒主循环走正常退出、atexit 兜底（`restore()` 幂等）。SIGKILL/SIGSEGV 无法覆盖，是终端
程序的共同边界。

### 0.5 测试范围

`test/tui` 只覆盖**当前里程碑**（现在是 §3.8 选择与复制、§3.9 定时器）：选区直接驱动真实的
Scrollback + Document 断言源文本与反色，定时器用管道子进程观察 Runtime 的诊断计数，OSC 52
用 pty 进程级用例由父进程扮演终端，不做烟雾测试。L1–L7、M1、M2、M4、M5 的用例已随各自
里程碑完成而移除；后续每个里程碑按本文各节的「验收」重新补齐该里程碑的用例，并移除上一
里程碑的用例。

---

## 一、结论

渲染内核的方向正确，性能上不弱于 OpenCode 所用的 OpenTUI：单元格网格 + 行内差分 +
SGR 游程 + DEC 2026，按需出帧且静止零输出，L5 的块模型与字节锚点对重折稳定。
这部分**不改**。

差距不在内核，而在原先刻意划掉的边界与几处不成立的假设：

| # | 问题 | 性质 |
| --- | --- | --- |
| 1 | 业务线程 `post()` 与渲染线程共用 `state_` 锁，渲染持锁做布局/物化/光栅化；大文档 resize 期间流式回调全部等锁 | 违背 §0.2 ①「业务线程永不被界面阻塞」 |
| 2 | 解码器不认 OSC/DCS 字符串：终端应答（如 OSC 11 背景色）被拆成 Alt 键 + 文本混进输入框；不认 kitty 键盘协议（`CSI u`），Shift+Enter 与 Enter 不可区分；能力探测只靠环境变量猜测 | 实测缺陷 + 能力缺失 |
| 3 | 没有浮层：命令面板、对话框、补全弹窗、toast、权限确认无处安放；且与「干净控件跳过 + back 复制 front」冲突，浮层关闭会留残影 | 结构缺失 |
| 4 | 鼠标事件只走焦点链，不按坐标分发 | 结构缺失 |
| 5 | 开鼠标即失去终端原生选择，没有应用内选择与复制 | 结构缺失 |
| 6 | Document 只能尾部追加，工具状态回写、撤销、并行输出无法表达 | 接口缺失 |
| 7 | 增量折行（§0.2 ③）只对纯文本成立；agent 输出主体是 Markdown，渲染器只能退化为每帧全文重排 | 假设不成立 |
| 8 | 缺通用定时器、终端挂起/恢复、快捷键与命令层、语义主题 | 基础设施缺失 |
| 9 | Unicode 宽度表手写未生成；intern 表无界增长 | 正确性/资源 |

---

## 二、OpenCode 能力 → 框架原语

| OpenCode 能力 | 需要的框架原语 | 本文章节 |
| --- | --- | --- |
| 流式输出不卡 UI、UI 不拖慢生成 | 更新通道 | §3.1 |
| Shift+Enter 换行、可靠的修饰键 | kitty 键盘协议 | §3.2 |
| 亮/暗主题自动适配、按终端能力降级 | 终端应答解析 + 能力握手 | §3.2 §3.3 §3.12 |
| 命令面板、模型/会话选择、权限确认、toast、@ 与 / 补全 | 层栈与浮层 | §3.4 |
| 滚轮滚动指针下的面板、点击 | 鼠标命中 | §3.5 |
| 工具调用状态变化、`/undo` `/redo`、子任务并行输出 | 文档变更 API | §3.6 |
| Markdown、代码高亮、表格、diff | 流式 Markdown 与块渲染器 | §3.7 |
| 鼠标开启下仍能选择复制 | 选择模型 + OSC 52 | §3.8 |
| leader 键超时、toast 自动消失、双击判定 | 定时器 | §3.9 |
| `/editor` 调用 `$EDITOR`、Ctrl+Z | 终端挂起/恢复 | §3.10 |
| 可配置快捷键、leader、命令面板 | 快捷键与命令层 | §3.11 |
| 主题切换 | 语义主题令牌 | §3.12 |
| CJK / emoji 不错位 | 生成的 Unicode 表 | §3.13 |

---

## 三、设计

### 3.1 L7 更新通道：业务线程零阻塞

**现状**：`post(fn)` 在 `state_` 锁内执行 fn；渲染线程在 `frame_locked()` 全程持有同一把锁。
控件树被两类线程共享，锁的持有时间由渲染决定。

**改为**：控件树与 Document **只属于渲染线程**，不再有 `state_` 锁。业务线程与渲染线程之间
只剩一个更新队列：

```cpp
class Runtime {
public:
    // 任意线程。fn 被放入队列，稍后在渲染线程上执行（不是在调用线程上）。
    // 节点在锁外分配，临界区只有一次尾插（两个指针写），最坏 O(1)。
    // 渲染线程上调用时直接执行（保持现有的重入语义）。
    void post(std::function<void()> fn);
private:
    struct Task { std::function<void()> fn; Task* next; };
    std::mutex queue_mutex_;     // 只保护 inbox_head_ / inbox_tail_
    Task* inbox_head_ = nullptr; // 业务线程尾插，渲染线程整条摘走
    Task* inbox_tail_ = nullptr;
};
```

渲染线程每次被唤醒：锁 `queue_mutex_` → 摘走整条链（头尾置空）→ 解锁 → 依序执行并释放
节点 → 检查控件树是否失效。锁内只有指针改写。

**为什么是链表不是 `vector` + swap。** 后者的 `push_back` 只是**摊还** O(1)：一次慢帧期间
队列能涨到十万级，那一次扩容要搬走全部已排队的 `std::function`，实测单次 `post` 因此达到
1.5ms，正好违反本节的验收。链表尾插与已积压的队列长度无关，最坏情况也是 O(1)。

**应用层边界**：业务代码（agent、网络、工具执行）不持有任何 widget 指针，只通过 `post`
提交**领域更新**（例如「消息 m 追加文本 chunk」「工具调用 t 状态变为 completed」）。
领域更新到控件操作（Document 的 open/append/replace）的映射由渲染线程上的会话视图适配器完成。
同一条领域更新流可以同时驱动非 TUI 输出模式。

**语义变化与迁移**：
- `post` 由「同步执行完再返回」变为「入队即返回」。依赖同步读取状态的代码（现有
  `app_test.cpp` 里 `rt.post([&]{ seen = ...; })` 后立即读取）改用 `std::promise`
  在 fn 内 `set_value`，调用方 `get()` 等待。
- `set_focus`、`after`/`every`（§3.9，取代原 `on_tick`）等配置接口的线程约束改为「run 之前
  或渲染线程」，与现状一致。
- §0.2 ① 与 §0.3 的管线图同步改写：不再有 `state_mutex`，纪律变为「队列锁内只改指针」。

**验收**（`test/tui/app_test.cpp`）：
- 渲染线程被一个慢帧（> 50ms）占住期间，业务线程连续 `post` 的单次耗时最大值 < 1ms。
- 1000 次/秒 `post` 持续 1 秒：帧数 ≤ 1000/16 + 余量，最终内容正确（保留现有合帧用例）。
- 渲染线程上重入 `post`（事件处理器、定时器回调内）不死锁（保留现有用例）。

慢帧由一个在 `render()` 里忙等的测试控件制造，不是靠灌 50 MB 文档触发重折。后者慢在制造
条件（-O0 下一帧 12 秒、RSS 300 MB），而且大块 `mmap`/`mremap` 会持有内核的地址空间写锁 ——
业务线程只要有一次 `malloc` 需要扩堆就被挡住，实测尖峰 10ms。那测的是内存子系统，不是
框架的锁设计，会把这条验收变成一个与实现无关的偶发红灯。

---

### 3.2 L6 解码器：终端字符串与 kitty 键盘协议

#### 3.2.1 终端字符串（OSC / DCS / APC / PM / SOS）

**实测缺陷**：`\e]11;rgb:1e1e/1e1e/1e1e\e\\` 被解成 Alt-`]` + 文本 `11;rgb:1e1e/1e1e/1e1e` +
Alt-`\`；XTVERSION 的 DCS 应答同样泄漏。任何终端查询都会污染输入。

**歧义**：`\e]` 同时是 OSC 引导符与用户按下的 Alt-]。不能无条件按 OSC 解析，否则用户按
Alt-] 后的所有输入都会被当成字符串吞掉，直到出现终止符。

**规则**：解码器引入**应答窗口**。

```cpp
class Decoder {
public:
    // L7 发出查询时打开，收到哨兵应答（DA1）或超时后关闭。
    void set_reply_window(bool open) noexcept;
};
struct Event {
    enum class Kind { text, key, mouse, paste, resize, focus, reply };
    // Kind::reply：text = 完整序列体（不含引导符与终止符），reply_type 区分来源
    enum class ReplyType : uint8_t { csi, osc, dcs, apc } reply_type{};
};
```

- 窗口打开时：`\e]` `\eP` `\e_` `\e^` `\eX` 开始字符串，读到 BEL（仅 OSC）或 ST（`\e\\`）
  为止，产出一个 `reply` 事件；带私有标记 `?` `>` `=` 的 CSI（DA1 `\e[?62;22c`、
  DECRQM `\e[?2026;2$y`、kitty 查询 `\e[?1u`）也产出 `reply` 而不是丢弃。
- 窗口关闭时：保持现有行为（`\e]` = Alt-]，私有 CSI 整体丢弃）。
- 字符串上限 1 MiB，超出部分丢弃但仍读到终止符。
- 窗口内的不完整字符串不参与 40ms Esc 超时；窗口关闭时仍未终止的字符串整体丢弃。

#### 3.2.2 kitty 键盘协议

握手确认支持后（§3.3），进入时推入 `\e[>1u`（flag 1：消歧转义码），还原路径弹出 `\e[<u`
（`Terminal::restore` 逆序栈里加一项）。

- 解码 `CSI code[:alternate];mods[:event] u` 与带修饰的 `~` / 字母终止键；`:` 子参数不再
  一律判为不可识别。
- `code` 13/9/127/27 映射为 enter/tab/backspace/escape，其余可打印码点产出
  `Kind::key` + `text`（有修饰）或 `Kind::text`（无修饰）。
- `Mods` 增加 `super = 1 << 3`；kitty 修饰位 hyper/meta/caps/num 忽略。
- 只用 flag 1：不请求按键释放/重复事件，不改变文本输入路径。

**验收**（`test/tui/input_test.cpp`）：
- 窗口打开：OSC 11（ST 与 BEL 两种终止）、DCS XTVERSION、DECRQM、DA1 各产出恰好一个
  `reply` 事件，零 `text`/`key` 事件。
- 窗口关闭：`\e]x` 仍为 Alt-] + x（现有语义不变）。
- `\e[13;2u` → enter + shift；`\e[97;5u` → key ctrl + "a"；`\e[1;5:1A` → up + ctrl。
- ESC 密集模糊测试的字母表加入 `] P \\ u : \x07`，整喂 / 逐字节 / 随机分块结果一致。

---

### 3.3 L1/L7 能力握手

**现状**：`probe_caps()` 只看 `TERM` / `COLORTERM`，同步输出与鼠标能力靠猜。

**改为**：环境变量结果作为初始值，`Runtime::run()` 开始时发出一组查询、打开应答窗口，
**不阻塞首帧**，应答到达后升级能力：

| 查询 | 应答 | 用途 |
| --- | --- | --- |
| `\e[?2026$p` | `\e[?2026;{1,2}$y` = 支持 | `caps.synchronized` |
| `\e[?2027$p` | `\e[?2027;{1,2}$y` = 支持 | 字素簇宽度模式（§3.13） |
| `\e[?u` | `\e[?{flags}u` = 支持 | 推入 kitty 键盘 flag 1（§3.2.2） |
| `\e]11;?\e\\` | `\e]11;rgb:RRRR/GGGG/BBBB` | 背景亮度 → 亮/暗主题（§3.12） |
| `\e[c`（DA1，最后发） | `\e[?...c` | **哨兵**：收到即关闭应答窗口 |

- DA1 所有终端都会应答，且终端按顺序应答，因此 DA1 之前没收到的查询视为不支持。
- 1 秒内没收到 DA1（管道、异常终端）→ 关闭窗口，保持初始值。
- `Terminal::Caps` 由渲染线程写入，新增 `kitty_keyboard`、`grapheme_width`、
  `background`（`std::optional<Color>`）。
- tmux 内部分查询由 tmux 自己应答，结果以应答为准；不做 tmux 透传包装。

**验收**（pty 用例，父进程在主端扮演终端）：
- 父进程读到查询后写回全部应答：子进程 caps 升级、主端收到 `\e[>1u`、输入框无杂字。
- 父进程只回 DA1：caps 保持初始值，窗口在 DA1 到达时关闭，随后按 Alt-] 仍产出 Alt-]。
- 父进程不回应：1 秒后窗口关闭，其间与之后的输入正常。

---

### 3.4 L3 层栈与浮层

#### 3.4.1 结构

根控件由 `Container` 改为 `LayerStack`：一个基础层 + 按 z 序排列的浮层。

```cpp
enum class Placement : uint8_t {
    center,        // 居中，常用于对话框、命令面板
    top_right,     // toast
    above_point,   // 贴在某点上方，空间不足翻到下方（补全弹窗贴光标）
    at_point,
};

class LayerStack : public Widget {
public:
    explicit LayerStack(std::unique_ptr<Widget> base);
    // 返回 id；尺寸取 overlay.measure(可用尺寸) 并夹到屏幕内。
    uint32_t push(std::unique_ptr<Widget> overlay, Placement p, Point point = {});
    void move(uint32_t id, Placement p, Point point = {});
    std::unique_ptr<Widget> remove(uint32_t id);
    Widget* hit(Point screen) const noexcept; // §3.5
};
```

浮层的矩形在屏幕坐标系下计算，`screen_origin()` 对浮层直接返回其矩形原点。

#### 3.4.2 损伤传播

保留「干净控件跳过 + back 复制 front」的优化，用**损伤矩形**保证正确性。每帧：

1. `damage` = 本帧移除、移动、改变尺寸的浮层的**旧**矩形。
2. 基础层中屏幕矩形与 `damage` 相交的控件 `invalidate()`（`Container` 递归，按
   `screen_origin()` 判定相交）。
3. 渲染基础层；`Container` 记录本帧实际重画的子项屏幕矩形，汇总为 `painted`。
4. 按 z 序遍历浮层：浮层失效，或与 `painted ∪ damage` 相交 → 重画，并把其矩形并入 `painted`。

规则 4 保证基础层重画的内容不会盖住仍然打开的浮层；规则 1–2 保证浮层关闭后下面的内容被补画。

**不做**透明度与 alpha 混合：浮层不透明，背景变暗效果由浮层自己画整屏遮罩实现（遮罩是一个
全屏浮层，代价是一次全屏重画，只在打开/关闭时发生）。

#### 3.4.3 与路由、焦点的衔接

```cpp
// L7：打开浮层并（可选）接管输入与光标；关闭时恢复打开前的焦点与光标来源。
uint32_t Runtime::open_overlay(std::unique_ptr<Widget> w, Placement p, Point point,
                               EventHandler* modal, Widget* cursor_source);
void Runtime::close_overlay(uint32_t id);
```

`point` 与 `LayerStack::push` 同义（`above_point` / `at_point` 需要）。`modal` 压入现有模态栈；
关闭顺序不是后进先出时，按 L6 已有的空槽语义处理；光标来源同理 —— 先关的浮层若是上层浮层
记下的恢复点，恢复点改接到先关浮层自己的恢复点，不会恢复成已销毁的控件。

**验收**（`test/tui/layout_test.cpp`、`frame_test.cpp`）：
- **增量 == 全量**：随机序列（打开/移动/关闭浮层、基础层控件随机失效、改变尺寸）每一步后，
  增量路径得到的 back 与对同一棵树 `invalidate_tree()` 后全量渲染的结果逐格相同。
  这是本节的核心断言。
- 关闭一个覆盖输入框的对话框后，差分输出只包含对话框原矩形内的行。
- `above_point` 空间不足时翻到下方，矩形始终在屏幕内。

---

### 3.5 L7 鼠标命中

**现状**：鼠标事件与键盘事件一样走「模态栈 → 焦点 → 全局」。

**改为**：键盘事件路由不变；鼠标事件由 L7 按坐标分发。L3/L4 仍然不感知事件，控件与处理器的
对应关系登记在 L7：

```cpp
void Runtime::bind_mouse(Widget& w, EventHandler& h);
void Runtime::unbind_mouse(Widget& w);
```

分发顺序：

1. **捕获**：按下事件被某处理器消费后，直到释放前，该按钮的移动与释放事件都直接交给它
   （拖拽选择、拖动滚动条），不论指针是否移出。
2. **模态**：栈顶模态处理器对应的浮层若不包含指针位置，事件被吞掉（点击对话框外部不穿透；
   是否关闭对话框由处理器自己决定，它会收到一个 `outside = true` 的事件）。
3. **命中链**：`LayerStack::hit` 找到最上层包含该点的最深控件，从它沿父链向上，
   第一个绑定了处理器且消费的为止。
4. **全局**处理器。

`Event::Mouse` 增加 `x`、`y`（相对命中控件左上角，由分发器逐级改写）与 `outside`。

**验收**（`test/tui/app_test.cpp`，直接向 Runtime 注入事件的用例）：
- 输入框有焦点时，滚轮落在滚动区坐标 → 滚动区处理器收到，输入框收不到。
- 对话框打开时点击对话框外部 → 基础层处理器收不到，模态处理器收到 `outside = true`。
- 按下后拖出控件矩形并释放 → 移动与释放事件全部交给按下时的处理器。

---

### 3.6 L5 文档变更 API

```cpp
class Document {
public:
    bool replace(uint64_t id, std::string source); // 任意块；重置该块缓存
    bool set_meta(uint64_t id, std::string meta);  // 失效该块物化缓存
    size_t erase_from(uint64_t id);                // 删除 id 及其后所有块（/undo），返回删除数
};
```

- **多个 open 块**：取消「只有尾部块可以 open」的限制。每帧对所有脏块做增量计数，前缀和从
  最靠前的脏块索引开始重建，代价 O(块数 − 该索引)。工具调用与流式输出集中在尾部，实际代价
  是 O(最近若干块)；若性能测量显示中部更新成为瓶颈，再把前缀和换成 Fenwick 树，接口不变。
- **锚点规则**：锚点所在块被 `replace` → 字节偏移夹到新长度；被 `erase_from` 删除 →
  锚点移到删除点之前最后一块的末尾，若文档已空则贴底。
- `replace` 与 `append` 一样只改内存并置脏，折行在渲染时进行。

**验收**（`test/tui/document_test.cpp`）：
- 随机的 append / open / replace / set_meta / erase_from / close 序列之后，逐行渲染结果与
  用相同最终内容新建的 Document 完全相同（行数、每行 spans）。
- 中部块 `replace` 后锚点仍指向同一段内容；锚点块被删除后按上述规则落位，不越界。

---

### 3.7 L5 流式 Markdown 与块渲染器

#### 3.7.1 为什么不能在单个块里增量

增量折行依赖「已定行不会因追加而改变」。Markdown 不满足：未闭合的 `**` 会改变前文样式；
代码围栏打开后其后全部内容换成代码样式；表格列宽取决于所有行。`BlockRenderer` 只能返回
`stable_rows = 0`，于是**正在流式增长的那条消息每帧全文重排**，而这正是最热的路径。

#### 3.7.2 按块级结构切分

新增 `MarkdownStream`（L5，渲染线程使用），把一条流式消息切成多个 Document 块，
**只有最后一个块在增长**：

| 块级结构 | BlockKind | 块何时关闭 |
| --- | --- | --- |
| 段落、标题、列表、引用 | `markdown` | 空行，或下一行以其他块级结构开始 |
| 围栏代码（``` / ~~~，info 串存入 `meta`） | `code` | 遇到匹配的闭合围栏 |
| 表格（表头 + 分隔行） | `table` | 空行或不再以 `\|` 开始的行 |
| 分隔线 | `markdown` | 立即 |

- **不完整行立即显示**：未完成的行先追加到当前块。整行到达后若判定它开始了新结构（例如一行
  以 ``` 开头），用 `replace` 把它从当前块移除、关闭当前块、以这一行开新块。代价只与该行长度相关。
- **行内样式**（粗体、斜体、行内代码、链接）在 `markdown` 块内解析；该块重排代价受段落长度约束。
- **代码块**逐行着色，每行起始处缓存词法状态（是否在多行字符串/注释内），已定行 = 最后一行
  之前的所有行，因此代码块保持增量。
- **表格**每次变化整块重排（列宽依赖全部行）；列宽超出屏幕时按比例收缩，单元格截断加 `…`。
- `BlockKind` 增加 `markdown`、`table`，`k_block_kind_count` 同步更新。
- **块间距**：`Block` 增加 `margin_top`（块前空行数），由产生块的一方设置，`Document` 计入
  行数前缀和并在块前补空行，渲染器不感知。`MarkdownStream` 在消息内除第一块外每块设为 1
  （段落间距），第一块取构造参数 `first_margin`（消息之间的间距归应用）；源码里的空行不进块。
  没有内容行的块边距折叠为 0，流式新开的空块不会先冒出空行。边距行没有内容可锚：
  `location_of` 把它映射到下方块首，`Scrollback` 向上滚动时越过边距锚到上一块末行，视口
  顶行从不停在边距行上，也不会卡住。

#### 3.7.3 语法高亮

内置轻量词法高亮器：关键字、字符串、注释、数字、函数名、类型名，覆盖 C/C++、Python、
JavaScript/TypeScript、JSON、Bash、Go、Rust；未识别的语言按纯文本。**不引入 tree-sitter**
（外部依赖与语法文件体积都与本框架的技术约束冲突）。

**验收**（`test/tui/document_test.cpp`）：
- 一份覆盖段落、标题、列表、嵌套引用、多个语言的代码块、表格、分隔线的 Markdown 语料，按
  随机大小分块喂入 `MarkdownStream`，得到的块序列（kind、source、meta）与整份喂入相同，渲染
  行逐行相同。
- 用计数型渲染器包装：流式喂入长代码块时，每帧渲染器处理的字节数 ≤ 最后一行长度 + 常数；
  流式喂入长段落时 ≤ 当前段落长度。
- 块间距：消息内块之间恰好空 1 行，空块不占边距；逐行上下滚动经过边距时视口顶行不停在
  边距行上、不卡住。

---

### 3.8 选择与复制

- **选择模型**：`Scrollback` 持有 `Selection{Location anchor, Location head}`。用逻辑位置
  （块 id + 字节偏移）表示，改变宽度后选区仍覆盖同一段内容。
- **交互**：鼠标拖拽选择（依赖 §3.5 的捕获）；双击选词、三击选逻辑行（依赖 §3.9 定时器判定
  连击间隔 400ms）；释放时复制，可配置关闭。拖拽是首选的选择方式：应用开启鼠标上报（滚轮、
  点击）后终端不再做原生选择，不按修饰键直接拖拽只能由应用接管；原生选择（Shift + 拖拽）
  按屏幕格复制，软折行处会插入换行、带上装饰、只能选当前视口。鼠标上报用 1002（按键事件
  跟踪）：1000 不报按住时的移动，拖拽收不到。
- **绘制**：选区在 `Scrollback::render` 时叠加反色，不写进 Document 的物化缓存；选区变化只
  失效滚动区视图。
- **复制内容**：Document 中两位置之间的**源文本**（Markdown 原文），不是屏幕字符，因此软
  折行不会插入换行。
- **剪贴板**：L1 新增 `Terminal::set_clipboard(std::string_view)`，写 OSC 52
  `\e]52;c;<base64>\a`，由渲染线程写出（L7 经 `Runtime::set_clipboard` 转发）。事件处理器
  本身就在渲染线程上，调用即写出、不等下一帧 —— 选区不变时释放鼠标不会出帧，等帧会丢复制。
  上限 1 MiB，超出时按 UTF-8 边界截断、返回 false 由应用层提示。tmux 需要 `set-clipboard on`，
  属使用方配置。
- **落点**：`Span::src` 记录每段显示文本的源偏移（装饰为 `k_no_src`），据此把屏幕坐标换算
  成逻辑位置（`Document::location_at`）、逐字素判定反色；`Document::text_between` /
  `word_around` / `line_around` 提供复制与连击扩展；`Scrollback` 持有选区（`hit` / `select` /
  `clear_selection` / `selected_text`）；L7 的 `ScrollbackMouse` 处理器把拖拽、连击、滚轮
  翻译成上述调用，经 `bind_mouse` 绑到滚动区。跨块复制补齐块间换行，有上边距的块前空一行；
  代码块的围栏行不在 source 里，复制内容不含围栏。

**验收**：
- 跨软折行的选区复制出源文本，不含软折行处的换行（`document_test.cpp`）。
- 选择后改变宽度，选区覆盖的字节范围不变（`document_test.cpp`）。
- pty 用例：拖拽选择后主端收到的 OSC 52 负载 base64 解码后等于源文本（`app_test.cpp`）。

---

### 3.9 L7 定时器

```cpp
using TimerId = uint64_t;
// 渲染线程调用；业务线程经 post 间接调用。回调在渲染线程执行。
TimerId Runtime::after(std::chrono::milliseconds delay, std::function<void()> fn);
// 回调返回 false 即取消，用于动画。
TimerId Runtime::every(std::chrono::milliseconds period, std::function<bool()> fn);
void Runtime::cancel(TimerId id);
```

- 最小堆存到期时刻；poll 超时取堆顶、Esc 超时、合帧余量三者的最小值；堆空且无其他等待时
  无限期阻塞，保持静止零唤醒。
- **`on_tick` 删除**，动画改用 `every`：回调返回 false 的暂停语义与现状一致；「post 或输入后
  重新挂上」的隐式行为不再需要，启动动画的代码显式调用 `every`。

**验收**（`test/tui/app_test.cpp`）：
- 多个 `after` 按到期顺序执行；`cancel` 后不执行；`every` 返回 false 后不再执行。
- 没有定时器时静置 300ms：零唤醒、零帧（改写现有 `idle_ui_neither_ticks_nor_renders`）。
  唤醒次数由诊断计数 `Runtime::wakeups()`（poll 每返回一次加一）观察，与 `frames()` 并列。

---

### 3.10 L1/L7 终端挂起与恢复

**现状**：`Terminal::restore()` 用 `restored_` 保证只执行一次，无法再次进入。

```cpp
class Terminal {
public:
    void suspend() noexcept; // 按还原栈逆序退出界面模式，termios 还原；信号处理器保留
    void resume();           // 重新进入：备用屏、raw、以及挂起前开启的鼠标/焦点/粘贴/kitty 模式
};
class Runtime {
public:
    // 渲染线程执行：suspend → fn（例如 fork/exec $EDITOR 并等待）→ resume →
    // front 置为未知状态 → 查尺寸 → 整屏重画。期间 post 在队列中累积。
    void run_external(std::function<void()> fn);
    // Ctrl+Z：suspend → raise(SIGTSTP) → 进程被 SIGCONT 继续后 resume + 整屏重画。
    void suspend_process();
};
```

- `restore()` 保留为进程退出路径（atexit、退出类信号），与 `suspend()` 共用逆序栈，但只有
  `restore()` 是终态。
- raw 模式下 Ctrl+Z 以字节 0x1A 到达，由全局处理器决定是否调用 `suspend_process()`。

**验收**：
- pty 用例：`run_external` 内向 stdout 写一行文本；主端输出中该文本位于 `\e[?1049l` 与下一个
  `\e[?1049h` 之间，恢复后的第一帧包含每一行的整行写出。
- Ctrl+Z 与 `$EDITOR` 的实际体验在真实终端目视确认（孤儿进程组中的 SIGTSTP 会被内核丢弃，
  无法在测试进程里验证）。

---

### 3.11 L7 快捷键与命令层

```cpp
struct Command {
    std::string id;       // "session.new"
    std::string title;    // 命令面板显示
    std::string category;
    std::function<void()> run;
    std::function<bool()> enabled; // 空 = 始终可用
};

class Keymap : public EventHandler {
public:
    void add(Command c);
    // 绑定串：以空格分隔的按键序列，"<leader>" 表示 leader 键。
    // 例："ctrl+p"、"<leader> n"、"shift+enter"、"escape"。
    bool bind(std::string_view keys, std::string_view command_id);
    void set_leader(std::string_view key, std::chrono::milliseconds timeout);
    const std::vector<Command>& commands() const noexcept; // 命令面板数据源
};
```

- `Keymap` 作为全局处理器安装。按下 leader 后它**压入模态栈**并启动超时定时器（§3.9），
  因此下一个按键不会先被输入框当作文本消费；匹配成功、不匹配或超时后弹出。
- 同一按键在不同上下文的归属仍由栈顺序决定（L6 原则不变）：对话框内的 Escape 由对话框处理器
  消费，不到达 `Keymap`。
- 绑定表由应用层从配置文件读取后调用 `bind` 覆盖默认值；框架只提供解析与分派。

**验收**（`test/tui/app_test.cpp`）：
- `"<leader> n"`：leader 后按 n 执行命令，输入框不出现 n；leader 后超时再按 n，n 进入输入框。
- 绑定串解析：`ctrl+shift+a`、`alt+enter`、`f5`、`<leader> ctrl+c` 正确；非法串 `bind` 返回 false。
- 冲突时后绑定覆盖先绑定。

---

### 3.12 语义主题令牌

L5 的 `Theme`（5 个槽位）改为全局语义令牌，所有渲染器与控件只引用令牌：

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

- 每套主题提供 `dark` 与 `light` 两组令牌。§3.3 取得背景色后按相对亮度（阈值 0.5）选择；
  取不到时默认 `dark`。
- 真彩色到 256 色的降级沿用 `present` 现有的量化逻辑，主题不需要为低能力终端单独配色。
- 主题文件的格式与加载属于应用层；框架只定义令牌与 `epoch` 失效规则（沿用 L5 现有约定）。

**验收**：修改令牌并递增 `epoch` 后，所有块的物化缓存失效，重排结果使用新样式
（`document_test.cpp`）。

---

### 3.13 Unicode 表与 intern 表

- **宽度表生成**：`grapheme.cpp` 中的宽度区间与字素属性表改为由 Unicode 数据文件
  （`EastAsianWidth.txt`、`emoji-data.txt`、`GraphemeBreakProperty.txt`、
  `DerivedGeneralCategory.txt`）生成，写入该文件内标记区段，注释记录 Unicode 版本与生成命令。
  按项目目录约定，生成脚本不入库。
- **emoji 宽度**：`Emoji_Presentation=Yes` 或后随 VS16 的簇宽度为 2。握手确认支持 mode 2027
  时开启 `\e[?2027h`，此时终端与框架使用同一套字素簇规则。
- **intern 表上限**：超长字素 intern 表超过 4096 项时，在下一帧前清空表、`invalidate_tree()`
  并强制整屏重画（front 中的旧索引随之失效）。

**验收**（`test/tui/grapheme_test.cpp`）：
- 生成表对 Unicode 官方 `GraphemeBreakTest.txt` 的用例切分结果一致；§0.1 L2 允许不实现的规则
  （Indic 辅音连缀 GB9c、前置字符 GB9b）涉及的用例按清单排除，清单写在测试里。
- 连续写入 5000 个不同 ZWJ 序列后 intern 表项数 ≤ 4096，且整屏内容与全量渲染一致。

---

## 四、明确不做

本节是框架边界的最终定义。

**保留**：
- 不做约束求解布局（flexbox / grid 全集）。fixed / content / flex + 层栈浮层覆盖全部界面需要。
- 不做单元格以下的差分。
- 不做通用控件库（按钮、表格、下拉的通用实现）。框架提供原语，界面控件属于应用层。
- 不支持非 ANSI 终端，不查 terminfo。
- 不做双向文本（BiDi）。

**修改**：
- 原「不做多窗格 / 分屏」→ 支持横向分栏（`Container` 横向，已实现）与浮层（§3.4）；
  **仍不做**窗口管理、可拖拽的分割线。

**原「不做」中移出、改为框架必须提供的原语**：
更新通道、终端应答与能力握手、kitty 键盘协议、层栈浮层、鼠标命中、选择与 OSC 52 复制、
文档变更 API、流式 Markdown 切分、定时器、终端挂起/恢复、快捷键与命令层、语义主题令牌。

**新增不做**：
- 透明度与 alpha 混合（浮层不透明，遮罩用全屏浮层实现）。
- 图片协议（kitty graphics / sixel）。
- tree-sitter 与任何外部语法库。
- OSC 8 超链接（Cell 没有存放链接 id 的空间，收益不足以改变 16 字节布局）。
- 按键释放/重复事件（kitty 协议只用 flag 1）。

---

## 五、实施顺序

每个里程碑结束时 `test/tui` 全部通过，并补齐该里程碑的验收用例。

| 里程碑 | 内容 | 依赖 | 理由 |
| --- | --- | --- | --- |
| M1 | §3.1 更新通道 | — | 决定上层所有代码的写法，越晚改迁移越大 |
| M2 | §3.2 解码器终端字符串 + kitty，§3.3 能力握手 | — | 修复实测的输入泄漏；主题与宽度模式依赖握手结果 |
| M3 | §3.9 定时器，§3.10 挂起/恢复 | M1 | 定时器是 M4–M6 多项功能的前提 |
| M4 | §3.4 层栈浮层，§3.5 鼠标命中 | M3 | 对话框、补全、toast 的载体 |
| M5 | §3.6 文档变更 API，§3.7 流式 Markdown 与高亮 | M1 | Markdown 切分依赖 `replace` |
| M6 | §3.8 选择复制，§3.11 快捷键命令层，§3.12 主题令牌，§3.13 Unicode 表 | M2–M5 | 依赖捕获、定时器、握手 |

M6 完成后 TUI 框架冻结；00 与本文同步更新为「描述当前实现」，之后只做应用层。

**必须在真实终端目视确认**（断言替代不了）：握手在 kitty / alacritty / wezterm / ghostty /
gnome-terminal / VS Code 内置终端 / tmux 中的实际应答与降级；tmux 下 OSC 52；Ctrl+Z 与
`$EDITOR` 的挂起恢复；emoji 与 mode 2027 在各终端的宽度表现；浮层打开关闭时有无闪烁。

---

## 参考

- [OpenTUI](https://github.com/anomalyco/opentui)
- [OpenTUI：渲染管线](https://opentui.com/docs/core-concepts/rendering-pipeline/)
- [OpenTUI：交互、焦点与选择](https://opentui.com/docs/core-concepts/interaction/)
- [OpenTUI：键盘输入](https://opentui.com/docs/core-concepts/keyboard/)
- [OpenTUI：ScrollBox](https://opentui.com/docs/components/scrollbox/)
- [OpenCode TUI 文档](https://opencode.ai/docs/tui/)
- [kitty 键盘协议](https://sw.kovidgoyal.net/kitty/keyboard-protocol/)
