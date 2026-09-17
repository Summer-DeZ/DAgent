# 终端 UI 框架设计

一套面向 C++23 / Linux 的终端界面框架。目标是把「高性能渲染」做成结构上的保证，
而不是靠局部优化去追。

**技术约束（刻意的选择）**

- 只依赖 POSIX（`termios`、`ioctl`、`poll`）与 ANSI/VT 转义序列。
  不引入 ncurses、notcurses、ftxui 等外部依赖。
- C++23，Linux。
- 单进程内多线程：业务逻辑在任意线程产生内容，界面在自己的线程渲染。

---

## 一、设计目标

| 目标 | 具体含义 |
| --- | --- |
| **渲染代价与变化量成正比** | 一帧的输出字节数取决于屏幕上真正变了什么，与内容总量、屏幕大小无关。静止画面上只有一个转圈符号在动时，每帧应当只写出那一个字符 |
| **业务线程永不被界面阻塞** | 产生内容的线程（网络回调、工具执行）只做 O(变化量) 的内存操作，永远不碰终端、永远不等 I/O |
| **突发合并** | 一秒内到达一千个 token，应当产生约 60 帧，而不是一千帧 |
| **大内容可承受** | 滚动区累积数十万行时，滚动、改变窗口大小、追加都不应退化成全量重算 |
| **文本正确** | CJK 宽字符、组合字符、emoji（含 ZWJ 序列）、制表符在任何列位置都不错位 |
| **状态可恢复** | 无论正常退出、异常还是信号，终端模式都被还原 |

---

## 二、核心原则

框架的全部结构都是为了守住这六条：

1. **单一真相是逻辑内容，不是屏幕上的字符。** 任何时候都能从逻辑内容按当前宽度
   重新推导出屏幕，反之不行。
2. **渲染是纯函数**：`(内容, 视口, 主题) → 单元格网格`。没有副作用，可重放，可断言。
3. **只有一个线程写终端。** 不存在"谁先写出去"的竞争，也就不需要排序机制。
4. **变更只改内存并置脏，绝不做 I/O。**
5. **输出通过差分产生**，不通过重绘产生。
6. **稳态帧路径零分配。** 所有缓冲复用，不在每帧构造临时容器。

---

## 三、分层架构

```
┌────────────────────────────────────────────────────────────┐
│ L7 Runtime    调度：置脏 → 合帧 → 单线程渲染                  │
├────────────────────────────────────────────────────────────┤
│ L6 Input      字节解码 → 事件 → 焦点链路由                    │
├────────────────────────────────────────────────────────────┤
│ L5 Document   大内容滚动区：两级折行、锚点、增量追加            │
├────────────────────────────────────────────────────────────┤
│ L4 Widget     视图：measure / render / on_event               │
├────────────────────────────────────────────────────────────┤
│ L3 Layout     布局：区域树 + 尺寸策略                         │
├────────────────────────────────────────────────────────────┤
│ L2 Surface    单元格网格 + 双缓冲 + 损伤差分   ← 性能核心      │
├────────────────────────────────────────────────────────────┤
│ L1 Terminal   终端抽象：能力、模式、尺寸、写出、恢复            │
└────────────────────────────────────────────────────────────┘
```

依赖只向下。L2 不知道 L3 以上的存在，L5 的滚动区只是 L4 的一个普通 widget
（但因为它是性能上最难的一个，单列一层说明）。

---

## 四、L1 Terminal：终端抽象

职责：进入/退出界面模式、探测能力、报告尺寸、把字节写出去。**不做任何绘制决策。**

```cpp
class Terminal {
public:
    struct Caps {
        bool truecolor      = false;  // COLORTERM=truecolor / 24bit
        bool synchronized   = false;  // DEC 2026 同步输出
        bool sgr_mouse      = false;  // 1006 扩展鼠标上报
        bool bracketed_paste= false;  // 2004
        bool focus_events   = false;  // 1004
    };

    Terminal();                        // RAII：构造进入，析构还原
    const Caps& caps() const noexcept;
    Size size() const noexcept;        // 每帧查询，ioctl 约 1µs
    void write(std::string_view bytes);// 唯一出口，由渲染线程调用
};
```

**进入时设置**（能力允许才设）：

| 序列 | 作用 | 为什么 |
| --- | --- | --- |
| `\e[?1049h` | 备用屏幕缓冲区 | 退出后完整还原用户原有终端内容 |
| `\e[?7l` | 关闭自动换行（DECAWM） | 框架自己控制换行。开着的话一行写满会自动折到下一行，整个网格坐标全乱 |
| `\e[?25l` | 隐藏光标 | 绘制期间隐藏，帧末再定位并显示，消除光标乱跳 |
| `\e[?2004h` | 括号粘贴 | 粘贴的多行文本被 `\e[200~ … \e[201~` 包裹，可与逐字输入区分 |
| `\e[?1000h\e[?1006h` | 鼠标 + SGR 扩展上报 | 滚轮、点击。1006 是必须的：传统模式坐标编码在 >223 列时溢出 |
| `raw` termios | 关闭行缓冲与回显 | 逐字节拿到输入 |

**退出时逆序还原。** 三条保障路径：

1. RAII 析构 —— 覆盖正常返回与异常（前提是异常被 `main()` 捕获，从而发生栈展开）。
2. `SIGINT` / `SIGTERM` / `SIGHUP` 处理器 —— 通过 self-pipe 唤醒主循环，走正常退出。
3. `atexit` 兜底。

`SIGKILL` / `SIGSEGV` 无法覆盖，这是终端程序的共同边界，不为此额外设计。

**尺寸变化**：不用 `SIGWINCH` 处理器（异步信号上下文能做的事太少）。
渲染线程每帧调一次 `ioctl(TIOCGWINSZ)`，尺寸变了就提升布局纪元（见 L3）。
一次 ioctl 约 1µs，60Hz 下完全可忽略，换来的是零信号复杂度。

**鼠标上报的代价**：开启后多数终端的文本选择与复制需要按住 Shift。
这是真实的可用性损失，应当做成可开关，而不是默认常开。

---

## 五、L2 Surface：单元格网格（性能核心）

这是整个框架性能的地基。**屏幕被建模为单元格网格，而不是行字符串。**

### 5.1 为什么是网格而不是行字符串

行字符串（每行一个 `std::string`，含 ANSI）看起来更简单，但：

- 两个 widget 的输出区域重叠或相邻时，无法正确合成 —— 字符串只能整行替换。
- 样式跨行时要手动重发 SGR，每个 widget 都得自己处理一遍。
- 宽字符占两列、组合字符占零列，字符串的"第 N 列"不是"第 N 个字符"，
  每次裁剪都要重新扫一遍。
- 差分只能做到整行粒度。

网格把这些问题一次性解决在一个地方。代价是内存，而这个代价很小：
300×100 的终端是 3 万个单元格，每个 20 字节 = 600 KB，双缓冲 1.2 MB。

### 5.2 数据结构

```cpp
struct Style {
    Color fg{};          // 4 字节：1 字节 tag(default/16/256/rgb) + 3 字节值
    Color bg{};
    uint16_t attrs{};    // bold dim italic underline blink reverse strike
};                       // 12 字节（含对齐）

struct Cell {
    // 字素簇。绝大多数是单码点、≤4 字节 UTF-8，直接内联。
    // 超长簇（emoji ZWJ 序列等）：text[0] = 0xFF，后 3 字节是 intern 表索引。
    char     text[4]{' '};
    uint8_t  width  = 1;   // 0 = 宽字符的右半格占位；1 = 半角；2 = 全角
    Style    style{};
};                         // 20 字节

class Surface {
public:
    void resize(int cols, int rows);          // 只在尺寸变化时分配
    void clear();                              // 填充空格 + 默认样式

    // 绘制原语。所有坐标越界自动裁剪，不 UB、不抛异常。
    void put(int col, int row, std::string_view grapheme, Style);
    // 写一段文本，按字素推进列，返回写到的列。自动处理宽字符与零宽字符。
    int  text(int col, int row, std::string_view utf8, Style);
    void fill(Rect, char32_t ch, Style);
    void hline(int row, int col0, int col1, Style);

    // 子区域视图：widget 只看到自己的坐标系，写不到外面去
    Surface view(Rect) noexcept;

private:
    std::vector<Cell> cells_;                  // 行主序，一次分配
    std::vector<uint8_t> row_dirty_;           // 每行一个脏标记
    int cols_ = 0, rows_ = 0;
};
```

**宽字符的表示**：一个占两列的字符写入 `(col, row)` 时，`cells_[col]` 存字素且
`width = 2`，`cells_[col+1]` 存 `width = 0` 的占位格。这样"第 N 列是什么"永远是
O(1) 查询，裁剪、差分、光标定位都不需要重新扫描。
写入时若覆盖了某个宽字符的一半，把另一半改写成空格 —— 这是必须处理的边界，
否则会残留半个字符。

**`Surface::view(Rect)`** 让 widget 拿到一个受限视图：坐标从 0 开始，
越界写入被裁掉。这是"widget 不会画到别人区域里"的结构性保证，
而不是靠约定。

### 5.3 差分与输出

渲染线程持有两块 Surface：`front_`（终端当前状态）与 `back_`（本帧构造结果）。

```cpp
void present(Terminal& term, Surface& back, Surface& front, std::string& out) {
    out.clear();                                  // 保留容量，零分配
    if (term.caps().synchronized) out += "\e[?2026h";   // 开始同步帧
    out += "\e[?25l";                             // 隐藏光标

    Style current = Style::invalid();             // 强制第一次发 SGR
    for (int row = 0; row < back.rows(); ++row) {
        if (!back.row_dirty(row)) continue;       // 整行未动，跳过
        auto [first, last] = diff_span(back, front, row);
        if (first > last) continue;               // 内容相同，跳过

        out += cup(row + 1, first + 1);           // \e[r;cH
        for (int col = first; col <= last; ++col) {
            const Cell& c = back.at(col, row);
            if (c.width == 0) continue;           // 宽字符右半格不单独输出
            if (c.style != current) {             // 样式游程合并
                out += sgr_delta(current, c.style);
                current = c.style;
            }
            out += c.grapheme();
        }
    }
    if (current != Style{}) out += "\e[0m";
    out += cup(cursor_row + 1, cursor_col + 1);
    out += "\e[?25h";                             // 显示光标
    if (term.caps().synchronized) out += "\e[?2026l";
    term.write(out);
    swap(front, back);
    back.clear_dirty();
}
```

四个关键优化，都在这一段里：

1. **行级脏标记**：`Surface::put` 写入时顺手标记该行。未被任何 widget 触碰的行
   连比较都不做。
2. **行内差分区间**：只重写 `[first_diff, last_diff]`，不是整行。
   改一个字符就只写一个字符。
3. **SGR 游程合并**：只在样式真正变化时发转义序列，并且发**增量**
   （`sgr_delta` 只发生改变的属性，而不是每次 `\e[0m` + 全套重设）。
   大段同色文本的样式开销摊薄到近似零。
4. **同步输出（DEC 2026）**：`\e[?2026h/l` 告诉终端"这一帧还没画完，先别刷新"，
   消除撕裂。不支持的终端会忽略这两个序列，无副作用。

**不需要 `\e[K`（擦除行尾）**：网格里每个单元格都有确定内容，
差分会把变成空格的位置显式写成空格。这同时绕开了"光标在最后一列时 `\e[K` 行为
依终端而异"这个经典坑。

**光标移动**：默认用绝对定位 `CUP`（约 8 字节）。相邻差分区间距离很近时，
可以用 `CUF`（右移）更省 —— 但这属于可选优化，先不做，因为每行至多一次定位，
开销已经很低。

### 5.4 字素与宽度

`Surface::text()` 需要把 UTF-8 切成**字素簇**（不是码点）并计算显示宽度。
这是每帧都在跑的热路径：

- **码点宽度查表**：`wcwidth` 依赖 locale 且实现质量参差。框架自带一张
  从 Unicode `EastAsianWidth.txt` 生成的区间表，二分查找，结果按码点
  memoize 在一个小数组里（BMP 直接数组索引）。
- **字素聚合**：处理组合记号（Mn/Me）、变体选择符（VS15/VS16）、
  ZWJ 序列、区域指示符（国旗）。这部分只需覆盖 UAX #29 的主要规则，
  不必完整实现。
- **制表符**：在写入时就展开成到下一个 tab stop 的空格。网格里不存 `\t` ——
  存了的话每次列计算都要回溯。

---

## 六、L3 Layout：布局

区域树 + 尺寸策略。**故意做得小**：够用，不做约束求解器。

```cpp
enum class Sizing {
    fixed,        // 固定 N 行/列
    content,      // 由 measure() 决定，可为 0（不占位）
    flex,         // 分配剩余空间，按 weight 分摊
};

struct Constraint {
    Sizing sizing = Sizing::content;
    int    value  = 0;   // fixed 的行数，或 flex 的权重
    int    min    = 0;
    int    max    = INT_MAX;
};

class Container : public Widget {           // vertical / horizontal
    void layout(Rect area) {
        // 1. fixed 直接占用
        // 2. content 调 measure(可用宽度)，按 min/max 夹取
        // 3. 剩余空间按 weight 分给 flex
        // 4. 不够分时按声明顺序逆序压缩，直到 min
    }
};
```

**布局纪元（layout epoch）**：布局不是每帧都算。只在以下情况重算：

- 终端尺寸变化
- 某个 widget 的 content 尺寸变化（由它自己 `invalidate_layout()` 声明）

其余情况下沿用上一帧的 `Rect`，直接进入渲染。这避免了每帧对整棵树
调用 `measure()`。

一个典型的聊天式界面：

```
Container(vertical)
├─ Scrollback      flex(1)        ← L5，占满剩余
├─ Activity        content(0..1)  ← 转圈符号 / 当前动作，空闲时 0 行
├─ Notice          content(0..1)  ← 提示条
└─ InputBox        content(3..N)  ← 边框 + 多行输入
```

以后要加审批面板、搜索栏、侧栏，就是往树里插一个节点，
不需要重新推导任何偏移量。

---

## 七、L4 Widget：视图

```cpp
class Widget {
public:
    virtual ~Widget() = default;

    // 在给定可用尺寸下，自己想要多大（Sizing::content 时被调用）
    virtual Size measure(Size available) const { return {}; }
    // 画到自己的 Surface 视图里。坐标系从 (0,0) 开始，越界自动裁剪。
    virtual void render(Surface&) = 0;

    // 内容变了但尺寸没变 → 只需重画
    void invalidate();
    // 内容变了且可能影响尺寸 → 需要重新布局
    void invalidate_layout();

    virtual bool focusable() const { return false; }
    // 有焦点的 widget 可以指定光标落点，由渲染器在帧末定位
    virtual std::optional<Point> cursor() const { return std::nullopt; }
};
```

Widget 不感知事件：`Widget::on_event` 这类接口不存在（保持 L3/L4
对事件零依赖）。事件由 L6 的 `EventHandler` 接口承接，"按键 →
widget 模型调用"的翻译层（如 InputBoxHandler）也在 L6。

**重画粒度**：`invalidate()` 只标记该 widget。渲染时，未失效且 `Rect` 未变的
widget 可以跳过 `render()` —— 因为 `front_` 里它那片区域的内容还是对的。
这一步让"只有转圈符号在动"的场景真正做到每帧只碰一行。

（实现上：`back_` 每帧从 `front_` 复制一份作为起点，再让失效的 widget 重画。
复制 600 KB 是一次 memcpy，比重新光栅化所有 widget 便宜得多。
若要更省，可以只复制未失效区域，但通常不必。）

---

## 八、L5 Document：大内容滚动区

滚动区是性能上最难的 widget，单独设计。难点：内容可以是几十万行、
尾部在高速增长（流式输出）、宽度会变（需要重新折行）、要能滚到任意位置。

### 8.1 内容模型：块，不是行

```cpp
struct Block {
    uint64_t    id;          // 单调递增，永不复用；锚点引用它
    BlockKind   kind;        // 纯文本 / 富文本 / 代码 / 差异 / 输出 …
    std::string source;      // 逻辑原文，未折行、未上色
    std::string meta;        // 标题、来源等结构化附属信息
    uint32_t    group = 0;   // 分组（例如并发子任务）
    uint8_t     depth = 0;   // 缩进层级
    bool        open = false;      // 仍在增长（只有最后一个块可以）
    bool        collapsed = false;
    uint32_t    collapsed_rows = 0;

    // ---- 派生缓存 ----
    int         cache_key = -1;     // 宽度 + 折叠状态 + 主题纪元
    size_t      row_count = 0;      // 计数级结果
    std::vector<Line> rows;         // 物化结果，未物化时为空
    size_t      stable_bytes = 0;   // 增量折行锚点（见 8.5）
};
```

**存逻辑原文而不是屏幕行**，是这一层所有能力的前提：改变宽度能重折、
折叠/展开能切换形态、裁剪按块而不会把一个代码块砍成两半。

块序列用 `std::deque<Block>`，头部裁剪 O(1)。

### 8.2 两级折行：计数与物化分离

折行有两种需求，代价差一个数量级：

- **要知道有多少行** —— 维护总行数、定位滚动位置时需要，**对所有块**都要。
- **要拿到行的内容** —— 只有**可见的那一屏**需要。

拆成两个函数：

```cpp
// 只数行，不分配任何字符串。扫描字素、累加宽度、遇断点 ++count。
size_t count_rows(std::string_view source, int width);

// 物化成带样式的行，结果缓存进 Block::rows
void materialize(Block&, int width, const Theme&);
```

宽度变化时：对所有块做一次**无分配**的 `count_rows` 扫描，
只对可见的块做 `materialize`。前者是纯内存遍历，后者只有一屏的量。

**物化缓存的驱逐**：只保留「可见窗口 ± 一屏」范围内块的 `rows`，
超出范围释放。滚动区内存因此有界，与历史长度无关。

### 8.3 前缀和 + 二分定位

`Document` 维护每个块的累计行数前缀和：

- 尾部追加：`push_back` 一个前缀值，**O(1)**
- 定位第 N 个物理行属于哪个块：二分，**O(log n)**
- 宽度变化：O(n) 重算一次（配合 8.2 的无分配计数）
- 头部裁剪：记录一个全局行偏移量，避免整体平移前缀和

### 8.4 锚点：对重新折行稳定的滚动位置

**不要用"距底部 N 个物理行"表示滚动位置。** 宽度一变，物理行数就变了，
用户正在看的内容会跳走。

```cpp
struct Anchor {
    uint64_t block_id;    // 块 id 稳定，不受裁剪和重折影响
    uint32_t row_in_block;
    bool     pinned_to_bottom = true;   // 贴底状态单独表示
};
```

贴底是一个独立状态而不是"偏移量为 0"：新内容到达时，贴底则跟随，
不贴底则**保持锚点不动**（并提示有 N 行新内容）。改变窗口大小时，
锚点指向的那一行仍然在视野里。

### 8.5 增量追加：流式内容的关键

流式输出的块（`open == true`）在每次追加时**不做任何折行**，
只 `source += chunk` 并置脏。

折行发生在渲染时，且是增量的：折行是严格从左到右的贪心过程，
在空格处回退的决策只看已经存在的字符 —— 因此**除最后一行外，
每一行的断点都由已有字符唯一决定，不会被后续追加改变**。
把这个边界记为 `stable_bytes`，每帧只重折 `source.substr(stable_bytes)`
里的最后一个不完整行。

于是：

- **追加一个 chunk：O(chunk 长度)**，与已生成内容的总长无关。
- **渲染一帧：O(最后一行)**，不是 O(全文)。

这一条是流式界面能不能撑住高速输出的分水岭。

### 8.6 块渲染器：可扩展点

不同 `BlockKind` 由不同的渲染器把 `source` 变成带样式的行：

```cpp
class BlockRenderer {
public:
    virtual size_t count(std::string_view source, int width) const = 0;
    virtual void   render(const Block&, int width, const Theme&,
                          std::vector<Line>& out) const = 0;
};
```

注册表按 `kind` 分派。新增一种内容形态（富文本、差异着色、语法高亮、
表格）只是实现一个渲染器并注册，**不触碰框架任何其他部分**。
这是这一层唯一预留的扩展点，也是唯一需要的。

---

## 九、L6 Input：解码与路由

**解码与语义严格分离。** 这是避免"按键归谁管"这类冲突的结构性办法。

### 9.1 解码器：字节 → 事件

一个增量状态机，喂进任意分块的字节流，吐出事件：

```cpp
struct Event {
    enum class Kind { text, key, mouse, paste, resize, focus };
    Kind kind;
    std::string text;          // text/paste 的内容（UTF-8）
    Key  key;                  // enter/tab/backspace/方向/功能键/page_up/…
    Mods mods;                 // ctrl/alt/shift
    struct { int button, col, row; bool press, motion; } mouse;
    Size size;                 // resize 的新尺寸（L7 合成事件时填入）
    bool focus_gained;         // focus：\e[I / \e[O
};
```

必须正确处理的几类：

- **UTF-8 跨读取边界**：一个多字节字符可能被 `read()` 从中间切开，
  解码器必须保留残尾。
- **CSI / SS3 序列**：`\e[` 与 `\eO` 两种前缀，参数可含 `;` 和 `<`，
  终止符是 `0x40..0x7e`。**无法识别的序列必须完整读到终止符再丢弃** ——
  否则残留字节会被当成普通文本插进输入框，这是终端程序最常见的 bug 之一。
- **SGR 鼠标**：`\e[<Cb;Cx;Cy(M|m)`。滚轮是 `Cb` 的 64/65。
- **括号粘贴**：`\e[200~ … \e[201~` 之间的内容整体作为一个 `paste` 事件，
  与逐字输入区分（粘贴的换行不应触发提交）。
- **序列内的异常字节**（CSI 与 SS3 同规则）：C0 照常产出按键（ECMA-48），DEL 忽略，
  ≥0x80 中止序列并按普通输入重新解析；序列未收完时已产出的 C0 随等待撤回，
  保证分块不变性。
- **旧式 X10 鼠标**：终端不支持 1006 时发 `\e[M` + 3 个原始字节，负载必须一起吞掉。
- **Esc 的歧义**：单独的 `Esc`、Alt 组合键与转义序列前缀无法从字节上区分。
  用短超时（约 40ms）判定，**覆盖所有以 ESC 开头的不完整单元**：
  孤立 ESC 串 → 每个一个 `Esc`；`\e[` / `\eO` 后无参数 → Alt-[ / Alt-O；
  已带参数的残缺序列 → 丢弃。ESC 串的规则：后随 `[`/`O` 时最后一个 ESC 是
  序列引导符、倒数第二个是 Alt 前缀（rxvt 的 `\e\e[A` = Alt-Up），其余各是一个 `Esc`；
  后随其他字节时最后一个 ESC 是 Alt 前缀。

### 9.2 路由：焦点链 + 处理器栈

事件从**栈顶**开始下沉，第一个返回 `true` 的消费它：

```
[ 模态处理器 ]   ← 搜索框、确认对话、滚动处理器（仅非贴底时压栈）等
[ 焦点 widget ]  ← 当前有焦点的
[ 全局处理器 ]   ← Ctrl-C、Ctrl-D、全局快捷键
```

"滚动处理器"不是一个独立的层，就是模态栈的一个普通条目：
滚动状态激活时压栈、贴底后弹出。"某个键在某种状态下归谁"不再需要
任何条件判断 —— **栈的顺序就是答案**。例如 `Home` 键：滚动处理器
在栈里时它跳到顶部，弹出后下沉给输入框跳到行首。
不需要在任何地方写 `if (scroll_offset > 0)`。

处理器在 `on_event` 里 push/pop 栈是合法操作（确认框消费 Esc 后
弹出自己，或连同下层模态一起关闭并销毁）。路由按下标自顶向下遍历，
下发期间的 `pop` 只把槽位置空、最外层下发结束时统一压实：

- 期间压栈的处理器不参与本次下发；
- 期间弹出的处理器立即不再被调用（包括本次还没轮到的），弹出后马上销毁是安全的；
- 不复制栈，路由零分配；嵌套下发（处理器里再次 `route`）时只在最外层压实。

---

## 十、L7 Runtime：调度与并发

```
业务线程（任意多个）                 渲染线程（唯一写终端者）
──────────────────────              ──────────────────────────────
lock(state_mutex)                    wait(cv, 直到 dirty 或 动画 tick)
  改 Document / widget 状态             enforce 最小帧间隔（约 16ms）
  invalidate()                       lock(state_mutex)
  dirty = true                         检查终端尺寸，必要时重新布局
unlock                                 失效的 widget → render 到 back_
cv.notify_one()                        取光标位置
                                       dirty = false
（零 I/O，O(变化量)）                 unlock
                                     diff(back_, front_) → 写终端
                                     swap(front_, back_)
```

**为什么是被唤醒驱动而不是固定频率轮询**：打字需要低延迟。按键立刻
`notify`，渲染线程马上醒来出帧。最小帧间隔只用来**合并突发** ——
一秒到一千个 token，也只出约 60 帧。

动画（转圈符号）另挂一个周期 tick，节奏可以比交互慢（例如 100ms），
两者共用同一个条件变量。

**关键纪律**：`state_mutex` 内**只做纯计算，绝不做 I/O**。
终端写入慢（窄终端、慢速 pty、输出被管道消费者阻塞）时，
持锁写入会把延迟直接回压到产生内容的线程上。

**唯一写者**顺带消除了"两个线程的输出交错或顺序颠倒"这一整类问题 ——
不需要序号、不需要排队。

---

## 十一、性能预算

| 场景 | 代价 |
| --- | --- |
| 追加一个流式 chunk | O(chunk 长度)。无折行、无分配、无 I/O |
| 一帧（仅动画在动） | 差分 O(1 行)，写出约 20 字节 |
| 一帧（流式输出中） | 重折最后一行 + 差分若干行，写出通常 < 200 字节 |
| 一帧（打字） | 重画输入框 + 差分 1 行 |
| 追加一个内容块 | O(该块文本)；不可见时只计数、不物化 |
| 改变窗口大小 | O(全部块文本) 一次无分配计数 + 一屏物化 + 一次全屏重绘 |
| 滚动一屏 | O(log n) 定位 + 一屏物化 |
| 裁剪历史 | O(1) 头部弹出 |
| 稳态每帧分配次数 | **0**（所有缓冲复用，`std::string::clear()` 保留容量） |

对照：朴素实现（每次变更重建整屏字符串并写出）在 200×50 终端上，
每帧约 10 KB 输出；本设计在静止与流式场景下是两个数量级的差别。

---

## 十二、接口总览

```
public/
  tui/terminal.hpp     L1  Terminal, Caps, Size
  tui/surface.hpp      L2  Cell, Style, Color, Surface, Rect
  tui/layout.hpp       L3  Sizing, Constraint, Container, Widget
  tui/widget.hpp       L4  具体视图（Text/Activity/Notice/InputBox）
  tui/document.hpp     L5  Block, Document, Anchor, BlockRenderer
  tui/input.hpp        L6  Decoder, Key, Mods, Event, EventHandler
  tui/app.hpp          L7  Runtime
private/
  tui/terminal.cpp  surface.cpp  present.cpp  grapheme.cpp
  tui/layout.cpp    document.cpp wrap.cpp     input.cpp     app.cpp
```

`grapheme.cpp` 含生成的 Unicode 宽度表，应当由脚本从
`EastAsianWidth.txt` / `DerivedGeneralCategory.txt` 生成而不是手写。

---

## 十三、验证方法

框架的大部分可以脱离真实终端断言验证 —— 这是"渲染是纯函数"带来的直接好处：

1. **折行等价性**：`count_rows(s, w)` 恒等于 `materialize(s, w).size()`。
   随机语料覆盖 CJK、组合字符、emoji ZWJ、超长单词、制表符、
   以及任意分块喂入（验证增量追加与一次性折行结果逐行相同）。
2. **网格不变量**：任何绘制序列之后，宽字符的 `width==2` 格后面
   必定紧跟一个 `width==0` 格；不存在孤立的 `width==0` 格。
3. **差分等价性**：把差分产生的字节序列应用到一块虚拟屏幕上，
   结果必须与 `back_` 逐格相同。这是整个渲染管线的核心断言，
   用随机帧序列跑。
4. **输出量**：静止场景下每帧 `out.size()` 应为常数级而非 O(W×H)。
5. **解码器**：对随机字节流（含截断的转义序列、跨边界的 UTF-8）
   不崩溃、不吞掉后续输入、不产生伪造事件。
6. **零分配**：稳态帧路径可用自定义 allocator 或 `malloc` 钩子断言分配次数为 0。

**必须在真实终端目视确认**（断言替代不了）：
改变窗口大小、光标落点、宽字符与 emoji 在行尾的表现、
鼠标上报开关的清理、以及在不同终端模拟器（xterm / alacritty / kitty /
tmux / VS Code 内置终端）下的表现差异。

---

## 十四、明确不做

划清边界，避免做成通用 TUI 工具库：

- **不做约束求解布局**（flexbox / grid 全集）。固定 + 内容 + 弹性三种策略
  覆盖纵向堆叠式界面的全部需要。
- **不做单元格级以下的差分**。行内区间差分已经把稳态代价压到常数级。
- **不做控件库**（按钮、表格、下拉、滚动条皮肤）。框架提供绘制原语与
  事件路由，控件是使用方的事。
- **不做多窗格 / 分屏**。区域树是纵向的。真需要时再做，
  但那应当是一个新决定，而不是现在为它预留抽象。
- **不支持非 ANSI 终端**。不做 terminfo 查询，只做能力探测 + 合理降级。
- **不做双向文本（BiDi）**。
