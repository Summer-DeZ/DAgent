# L6 输入层审查

审查对象：`src/public/tui/input.hpp`、`src/private/tui/input.cpp`、`test/tui/input_test.cpp`（未提交工作树，基于 `3c9150f`）。
每条 bug 都用独立探针程序（ASan + UBSan）实测复现。

**状态：8 项全部修复，复查发现的 3 项回归（A/B/C）与 1 项遗留（D）也已修复。**
修复后 TUI 测试 117 个全部通过；input + widget 测试在 ASan + UBSan 下全部通过；
逐项撤回修复（C0 撤回、ESC 计数、路由空槽）都会让对应测试失败。

> 「位置」列是首次审查时的行号，「现位置」列是修复后的行号。

## 一、首次审查的 bug（按严重程度排）

| # | 位置 | 问题 | 实测结果 | 状态 | 现位置 |
| --- | --- | --- | --- | --- | --- |
| 1 | input.cpp:475-481 | `EventRouter::route()` 用迭代器遍历 `stack_` 的同时调用处理器。处理器在 `on_event` 里 push/pop 会让 vector 重分配或元素移位，迭代器失效 | 压入 64 个处理器后 ASan 在 477 行报 `heap-use-after-free` | ✅ 已修复（首轮用快照，复查 A 发现仍有问题，改为空槽方案） | input.cpp:567-600 |
| 2 | input.cpp:428-430 | `pending_escape()` 只在缓冲全是 ESC 时为 true。Alt-[ 发 `\e[`、Alt-Shift-O 发 `\eO`，无限期等待，下一个按键被当成终止符丢弃 | `\e[` 后跟 `a` → 0 个事件；`\eO` 后跟 `k` → 0 个事件 | ✅ 已修复：超时覆盖所有以 ESC 开头的不完整单元 | input.cpp:490-535 |
| 3 | input.cpp:373-376 | Alt + 非法 UTF-8 字节时多跳过一个字节，同时破坏分块不变性 | 整喂 `"\e\xFF" "ab"` → 只有 `b`；分两次喂 → `a`、`b` | ✅ 已修复 | input.cpp:441 |
| 4 | input.cpp:159-195 | 终端不认 1006 时发旧式 X10 鼠标 `\e[M` + 3 个原始字节，后 3 字节被当成文本插入输入框 | `"\e[M a!z"` → 4 个 text 事件 | ✅ 已修复：连负载一起吞掉，不足 3 字节时等待 | input.cpp:295-302 |
| 5 | input.cpp:336-339 | 连续 ≥2 个 ESC 一律折叠成一个 Alt 修饰，多出的 ESC 被吞 | `\e\e\e[A` → 只有 Alt-Up；`\e\ex` → 只有 Alt-x | ✅ 已修复（首轮计数有误，见复查 B） | input.cpp:388-420 |
| 6 | input.cpp:254-258 | CSI 内的 C0 控制字节被吞掉；≥0x80 字节与 DEL 被当成终止符 | `\e[\r` → 0 个事件；`\e[你b` → 只有 `b` | ✅ 已修复（首轮引入重复产出，见复查 C；SS3 见复查 D） | input.cpp:205-226 |
| 7 | input.cpp:56-63 | `mods_from_param(0)` 把所有修饰位置上 | `\e[1;0A` → Shift+Alt+Ctrl | ✅ 已修复 | input.cpp:57-65 |
| 8 | input.cpp:485-490 | `InputBoxHandler` 不处理 `Kind::paste`，粘贴内容进不了输入框 | 读代码确认 | ✅ 已修复 | input.cpp:609 |

## 二、复查发现的问题（首轮修复后）

| # | 问题 | 实测结果 | 修复 |
| --- | --- | --- | --- |
| A | 快照遍历只防住了 vector 重分配：上层处理器弹出并销毁下层处理器后，快照里的悬空指针仍被调用；且每次下发复制一次栈（分配） | 上层 pop + `unique_ptr::reset` 下层 → ASan 在 route 中报 `heap-use-after-free` | ✅ 按下标遍历；下发期间 `pop` 只置空槽，最外层下发结束时压实（支持嵌套 `route`）。被弹出的处理器立即不再被调用，零分配 |
| B | 序列分支把 ESC 串的最后一个 ESC（序列自身引导符）也算作 Alt 前缀之外的多余 ESC，多出一个 Esc 按键；测试把错误语义写成了期望值 | `\e\e[A`（rxvt 的 Alt-Up）→ Esc + Alt-Up；`\e\e\e[A` → 两个 Esc + Alt-Up | ✅ 多余 Esc 个数改为 `esc_run - 2`；`flush_escape` 的残缺序列同规则；测试改正 |
| C | 序列未收完时已产出的 C0 事件没有撤回，缓冲下一轮重新解析时再产出一遍 | 整喂 `\e[\rA` → Enter、Up；逐字节 → Enter、Enter、Up；分三次 `\e[\r`/`\r`/`A` → 5 个 Enter + Up | ✅ 返回 wait 时撤回本序列扫描期间产出的事件；超时消解时补发残缺序列里的 C0（`\e[\r` → Alt-[ + Enter） |
| D | SS3 没同步修复：仍吞掉 C0、把 ≥0x80 当终止符 | `\eO\r` → 0 个事件；`\eO你b` → 只有 `b` | ✅ CSI 与 SS3 共用 `seq_control_byte` 规则 |

## 三、非最优实现

**逐码点 text 事件导致大段输入 O(n²)。** 解码器每个 UTF-8 单元产出一个 text 事件，`InputBox::insert` 每次都重算整行宽度。

✅ 已修复：同一次 `feed` 内连续的可打印字符合并为一个 text 事件；分块不变性改为约束"拼接后的文本"。

## 四、与设计文档不一致

✅ 已对齐 `docs/next-to-do/00-tui-framework.md`：

- §七：Widget 不感知事件，事件由 L6 的 `EventHandler` 承接；§十二 接口总览同步。
- §9.1：`Event` 增加 `size`（resize）与 `focus_gained`；补充序列内异常字节、X10 鼠标、ESC 超时与 ESC 串规则。
- §9.2："滚动处理器"并入模态栈；补充下发期间 push/pop 的语义（空槽方案）。
- 注释修正：input.cpp 鼠标修饰位 4/8/16；input.hpp 顶部关于 resize 的说明。

## 五、测试补充

| 测试 | 覆盖 |
| --- | --- |
| `fuzz_chunk_invariance` | 均匀随机噪声：整喂与随机分块结果一致（bug 3） |
| `fuzz_chunk_invariance_escape_heavy` | 从 ESC / 引导符 / C0 / 参数 / 终止符字母表抽样，整喂 vs 逐字节 vs 随机分块（复查 B、C） |
| `control_bytes_in_incomplete_sequence_not_duplicated` | C0 不重复产出（复查 C） |
| `ss3_control_bytes` | SS3 的 C0 / ≥0x80 / DEL / 超时（复查 D） |
| `x10_legacy_mouse` | X10 负载吞掉，含跨分块（bug 4） |
| `escape_timeout_covers_csi_intro` | Alt-[ / Alt-O / 残缺序列超时（bug 2） |
| `router_reentrant_stack_mutation` | 下发期间 push、自弹出（bug 1） |
| `router_pop_during_route_stops_pending_handlers` | 弹出尚未轮到的下层处理器、压实、嵌套下发（复查 A） |

## 六、已知边界（不修）

- `\e[M` 后 3 个字节总按 X10 鼠标负载处理：开启鼠标时，Alt-[ 紧跟 Shift-M 且 40ms 内再按 3 个键，这 3 个键会被吞掉。这是开启鼠标上报时协议本身的歧义。
- 超时消解需要调用方配合：`pending_escape()` 为 true 时 poll 带 `k_escape_timeout_ms` 超时，超时后调 `flush_escape()`。这由 L7 负责，尚未实现。
