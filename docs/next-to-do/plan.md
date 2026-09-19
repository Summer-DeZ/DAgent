# 执行计划

把核心拆成 C0–C6 七个阶段、41 个任务。每个任务写明内容、涉及的文件、依赖和完成标准；每个里程碑列出验收场景和
审核清单。部件的设计细节在各自的文档里，这里只讲**做的顺序**和**怎样算做完**。

---

## 1. 总览

```
C0 准备 ──► C1 最小循环 ──► C2 调度与权限 ──► C3 会话 ──► C4 交互界面 ──► C5 上下文 ──► C6 MCP
                                                    └──────────► C5（可提前）
```

| 里程碑 | 任务数 | 验收场景 | 做完之后 | 状态 |
| --- | --- | --- | --- | --- |
| C0 准备 | 3 | — | 知道用哪个网关验收；提示词第一版；验收材料就位 | **已完成** |
| C1 最小循环 | 10 | 1–4 | `dagent run "修好这个 bug"` 真的能改代码 | **已完成**（2026-09-19 审核通过） |
| C2 调度与权限 | 7 | 5–9 | 能放进 CI；读多的任务明显变快 | 下一步 |
| C3 会话 | 5 | 10–12 | 会话中断后能接着做 | |
| C4 交互界面 | 7 | 13–15 | 日常使用 | |
| C5 上下文管理 | 6 | 16–18 | 长任务不会撞上窗口 | |
| C6 MCP | 3 | 19–20 | 接入外部工具 | |

C4 和 C5 可以互换：C3 之后如果 run 模式的长任务已经撞窗口，先做 C5（C5 不依赖界面，`/compact` 留到 C4 再接）。

**约定**

- 每个任务做完都要构建通过、`-Wall -Wextra` 零警告。
- 验收只做真实运行（AGENTS.md）：真实模型、真实文件、真实进程；不写模拟模型。检测材料只放 `temp/core_check/`。
- 需要工具调用的场景用 DeepSeek（key 在 `.env.dev`），除非 C0.1 发现本地网关也能用。
- 任务完成标准里写「跑通」的，指在 `temp/core_check` 的对应脚本里跑过一次并看到期望结果。

---

## 2. C0 准备

| 任务 | 内容 | 产出 | 完成标准 |
| --- | --- | --- | --- |
| **C0.1** 网关实测 | 分别对 `127.0.0.1:10009`（ling-3.0-flash-int4，开发配置）、`127.0.0.1:10000`（qwen，已知会丢 `tools`）、DeepSeek 发一个带一个工具定义的请求，看 `prompt_tokens` 是否随工具定义变化、是否返回 tool_calls | 结论写进本节下方 | 知道哪个网关能跑工具调用 |
| **C0.2** 验收材料 | 建 `temp/core_check/`（§9 的布局）：`bugproj`、`batchproj`、`fault_proxy.py`、各里程碑的配置文件 | 目录与脚本 | `bugproj/build.sh` 能构建且确实暴露 bug；`fault_proxy.py pass` 转发 DeepSeek 正常 |
| **C0.3** 提示词第一版 | 按 [08-prompt §4.1](08-prompt.md) 写 `prompts/system.md`；`compact.md` 先留一个占位 | `prompts/system.md` | 用 `temp/tools_check` 里已有的最小循环换上这份提示词跑一次场景 1 的任务，模型的行为符合提示词（先读后改、改完构建） |

C0.1 的结论：`127.0.0.1:10009` 已关闭，本地网关不再可用；验收统一走 DeepSeek 官方网关
（`https://api.deepseek.com/v1`，key 在 `.env.dev` 的 `DEEPSEEK_API_KEY`）。实测支持工具调用与
`usage`（含 `prompt_cache_hit_tokens`），`temperature` 设 0。

**C1.10 实测结果**：场景 1 连跑三次全部一次完成，步数 7 / 8 / 7，工具调用 11 / 11 / 11，
耗时 7–9 秒。默认上限定为 `max_model_calls = 24`、`max_tool_calls = 35`（实测的 3 倍左右）。
`prompts/system.md` 一版通过，未再调整。


---

## 3. C1 最小循环

目标：run 模式下完整跑通一轮。只做串行调度、`auto` / `deny` 权限、会话**写入**（不做恢复）、text 输出。

| 任务 | 内容 | 文件 | 依赖 | 完成标准 |
| --- | --- | --- | --- | --- |
| **C1.1** Options 迁移 | `agent::Options` 及其子结构；app 的 `Config` 改用它；`Gateway` 加 `send_reasoning_content`、`include_usage`、`extra_body`，删 `enable_thinking`；`dagent.json` 补 `idle_timeout_seconds`、改 `system_prompt_file` | `agent/options.hpp`；`app/config.*`；`config/dagent.json` | — | 构建通过；`temp/app_check` 仍然跑通；`--set run.max_tool_calls=5` 能映射到新字段 |
| **C1.2** 事件类型 | Event 各结构、`TurnStatus`、`Approval`、`Decision`、`to_string` | `agent/events.hpp` | — | 构建通过（`to_json` 放到 C2.5） |
| **C1.3** Model | 累积、失败分类、重试、可取消等待 | `agent/model.*` | C1.2 | 场景 3、4 跑通 |
| **C1.4** Conversation | Entry、不变式、`build`、`validate`、`open_calls`、标准文本 T1–T9 | `agent/conversation.*` | — | 由 C1.8 的场景覆盖 |
| **C1.5** 提示词 | CMake 嵌入、覆盖文件、`render_system_prompt` | `agent/prompt.*`、`cmake/prompts.cpp.in`、`src/CMakeLists.txt` | C0.3 | 改 `prompts/system.md` 后重新构建即生效；模板语法错误时启动报错带行号 |
| **C1.6** 会话写入 | Recorder 的写入部分：全部记录类型（C5 的 prune / compaction 先不写）、`broken` 降级 | `agent/record.*` | C1.4 | 场景 2 |
| **C1.7** 基本权限 | Policy：路径分类、默认规则表、`automatic` / `deny` 两种模式（不含 ask、会话授权） | `agent/permission.*` | — | 由场景 1 覆盖；`--permissions deny` 时 edit 拿到 T6 |
| **C1.8** Agent 与串行调度 | `Agent::create`、`run_turn`（不含压缩、MCP、收尾步）、`dispatch` 串行版、`finish` | `agent/agent.*`、`agent/dispatch.cpp` | C1.3–C1.7 | 场景 1 |
| **C1.9** run 模式与 main | `run_headless`（text 输出）、`main`（run 模式；interactive 暂时提示「交互界面尚未实现」）、`make_setup`、可执行目标 | `agent/headless.*`、`agent/main.cpp`、`src/CMakeLists.txt` | C1.8 | `dagent run "…"` 能跑；`dagent --version` 正常 |
| **C1.10** 实测调参 | 用 DeepSeek 跑场景 1 三次，记录步数和调用数；定 `run` 上限默认值；打磨 `system.md` | `config/dagent.json`、`prompts/system.md` | C1.9 | 三次里至少两次一次性完成 |

### C1 验收场景

1. **修 bug**：`dagent run -C temp/core_check/bugproj "构建失败了，找出原因并修好"`。
   期望：模型用 grep / read 定位、edit 修改、bash 构建验证；`bugproj/build.sh` 通过；退出码 0；stdout 只有最终回复。
2. **记录完整**：场景 1 的会话目录里，`jq -c '{type, n: .payload.n}' events.jsonl` 显示
   `meta → system → user(0) → assistant(1) → tool(2) … → turn_end`；每个 assistant 的每个 tool_call 都有对应的 tool 记录；
   `grep -r "$DEEPSEEK_API_KEY"` 会话目录无结果。
3. **重试**：`fault_proxy.py` 依次用 `503x2`、`cut`、`nodone`、`retry-after`、`slow` 五种模式跑一个简单问题（[02-model §10](02-model.md)）。
   期望：前四种都在重试后成功，stderr 能看到重试提示；`cut` 的最终输出里没有重复的半句话；`slow` 模式下临时把
   `http.timeout_seconds` 设成 10 仍能完成。
4. **不重试的失败**：错误的 api key → 1 秒内失败，退出码 1，输出和日志里都没有 key；`base_url` 指向没人监听的端口 →
   重试用完后失败，退出码 1。

### C1 审核清单

- [x] `run_turn` 的每个退出点都走 `finish`（[04-turn §8](04-turn.md)）
- [x] Model 的等待可取消，没有 `sleep_for`
- [x] 流结束但没有 `Finish` 时重试，而不是当作成功
- [x] 进入 Message 的文本都是合法 UTF-8
- [x] 日志里没有请求体、api key、用户输入全文
- [x] 所有路径都从 `Setup::cwd` 传入，没有依赖进程当前目录

审核（2026-09-19）另外修了两处：一批里最后一个工具执行中被中断时，调度没有返回 interrupted（本轮会多走一步，
恰好到达模型调用上限时还会以 limit 结束）；未知工具不计入工具调用上限。前者由 `temp/core_check/interrupt_check`
验证（修复前 `status=limit`，修复后 `status=interrupted`）。

---

## 4. C2 调度与权限

目标：调度和权限做完整；run 模式具备放进 CI 的所有能力。

| 任务 | 内容 | 文件 | 依赖 | 完成标准 |
| --- | --- | --- | --- | --- |
| **C2.1** 并行组 | `parallel()`、挂起组、重新 prepare、分块 jthread、尽早提交 | `agent/dispatch.cpp` | C1.8 | 场景 5 |
| **C2.2** 上限与收尾步 | `budget`、T8、`hit_limit`、`grace` | `agent/agent.cpp`、`dispatch.cpp` | C1.8 | 场景 9 的上限部分 |
| **C2.3** 完整权限 | ask 分支、`Approval` 各字段、会话授权（含 bash 前缀规则）、沙箱不可用降级、`set_mode` | `agent/permission.*`、`dispatch.cpp` | C1.7 | 场景 6、7；前缀规则按 [06-permission §6.1](06-permission.md) 的表逐条验证（在 C4 的对话框里） |
| **C2.4** 权限参数 | `Args::permissions` 改 optional；优先级 `--permissions` → 配置 → `automatic` | `app/cli.*`、`agent/main.cpp` | C1.9 | 不传参数时配置的 `permissions: "deny"` 生效 |
| **C2.5** 输出格式 | `to_json(Event)`；json、jsonl 输出；退出码表 | `agent/events.cpp`、`agent/headless.cpp` | C1.9 | 场景 9 |
| **C2.6** 信号与管道 | sigmask + sigwait 线程；第二次 Ctrl+C `_exit(130)`；jsonl 写失败时停止 | `agent/main.cpp`、`agent/headless.cpp` | C1.9 | 场景 8；`dagent run --output jsonl … \| head -1` 正常结束 |
| **C2.7** 中断路径 | 核对 [04-turn §6](04-turn.md) 每一行：流式中、重试等待中、工具执行中 | `agent/agent.cpp`、`dispatch.cpp` | C2.1、C2.6 | 场景 8 |

### C2 验收场景

5. **并行与重新 prepare**：`dagent run -C temp/core_check/batchproj "一次性并行读取 a.cpp b.cpp c.cpp d.cpp，同时用 ls 列出目录；然后在同一次回复里先读 e.cpp 再修改它，再对 f.cpp 连续做两处修改"`。
   期望：日志（debug 级）里 4 个 read 和 ls 的开始/结束时间重叠；「read e → edit e」和「edit f → edit f」都成功；
   jsonl 里 `tool_finished` 的顺序与 tool_calls 一致。
6. **权限模式**：
   - `--permissions deny` 让它修 bugproj：edit 拿到 T6，模型在回复里说明需要用户做什么；退出码 0（策略拒绝不结束本轮）。
   - `--permissions auto` 让它「在 /tmp/dagent_outside.txt 写一行字」：工作区外写入被拒（T6）。
   - `--permissions auto` 让它「在 .mcp.json 里加一个 server」：受保护文件被拒。
7. **沙箱与网络**：`--permissions auto` 让它「用 curl 访问 https://example.com」：命令因为不联网而失败，工具结果里有
   沙箱提示，模型没有反复重试（不超过 2 次）。
8. **中断**：让它执行 `sleep 60`，5 秒后发 SIGINT。期望：1 秒内退出，退出码 130；会话记录里这次调用 `interrupted: true`，
   最后一条是 `turn_end`（`status: interrupted`）。再在模型流式输出长文时发 SIGINT：记录里 assistant 的 content 以 T1 结尾。
9. **输出与上限**：
   - `--output jsonl`：每行都能被 `jq` 解析；第一行 `session`，最后一行 `turn_ended`；每个 tool_call id 恰好一个 `tool_finished`。
   - `--output json`：对象字段齐全（[11-entry §4.2](11-entry.md)）。
   - `--set run.max_tool_calls=3` 跑场景 1：第 4 个调用起拿到 T8，模型在收尾步里给出进展总结，`status: limit`，退出码 1。

### C2 审核清单

- [ ] 并行组只含 [05-dispatch §3](05-dispatch.md) 规则 1 允许的调用
- [ ] 需要询问之前挂起组已经跑完
- [ ] 结果按原顺序提交；尽早提交
- [ ] 工作线程全部 join 后才返回
- [ ] Sink 的 run 模式实现加了锁
- [ ] 受保护文件的写入在 auto 模式下拒绝、在 ask 模式下不提供会话授权
- [ ] 沙箱不可用时只读 bash 也询问（交互）/ full_access + warn（auto）

---

## 5. C3 会话

目标：会话可以恢复，崩溃后也能恢复。

| 任务 | 内容 | 文件 | 依赖 | 完成标准 |
| --- | --- | --- | --- | --- |
| **C3.1** 回放重建 | `replay_into`：重建 entries、ordinal、回放事件；`Conversation::restore` | `agent/record.*`、`agent/conversation.*` | C1.6 | 场景 10 |
| **C3.2** 崩溃闭合 | T9、`turn_end_crashed`、一致性检查 | `agent/record.cpp`、`agent/agent.cpp` | C3.1 | 场景 11 |
| **C3.3** Agent::resume | 重新渲染 system、新 `system` 记录、模型变化提示 | `agent/agent.cpp` | C3.2 | 场景 10 |
| **C3.4** 入口参数 | `--resume`（含前缀匹配）、`--continue`、`sessions`、`trust` | `agent/main.cpp`、`agent/headless.cpp` | C3.3 | 场景 12 |
| **C3.5** 记录格式核对 | 对照 [09-record §3](09-record.md) 检查所有记录类型的字段；payload 里没有会被脱敏的键名 | — | C3.4 | 审核通过 |

### C3 验收场景

10. **中断后恢复**：场景 8 的会话 `dagent run -r <id前8位> "继续刚才的任务"`。期望：模型知道前面做到哪；它第一次 edit
    之前会重新 read（FileTracker 是空的）；下一次请求不 400。
11. **崩溃后恢复**：让它执行 `sleep 30`，期间 `kill -9` dagent。期望：`-r` 恢复后，记录末尾多出这个调用的 T9 和
    `turn_end{crashed}`；再恢复一次，不会重复追加；之后的对话正常。
12. **找会话**：在同一个项目里建 3 个会话、另一个目录建 1 个。期望：`dagent sessions` 只列出这个项目的 3 个，标题是各自
    第一条输入；`--continue` 恢复最新一个；给一个有歧义的 id 前缀时报错并列出候选；`dagent trust` 之后项目的
    `.dagent/config.json` 生效。

### C3 审核清单

- [ ] 回放出的历史 `validate()` 为空；不为空时报错而不是修补
- [ ] 恢复后的 ordinal 与恢复前连续
- [ ] 崩溃闭合写回了 JSONL
- [ ] FileTracker、会话授权都从空开始

---

## 6. C4 交互界面

目标：日常可用的全屏界面。

| 任务 | 内容 | 文件 | 依赖 | 完成标准 |
| --- | --- | --- | --- | --- |
| **C4.1** Shell 骨架 | 布局、主题（`ui.theme_file`）、Runtime、agent 线程、JobQueue、每轮 stop_source、退出流程、SIGTERM | `ui/shell.*`、`app/config.*`、`agent/main.cpp` | C3 | 能进入界面、输入、看到回复（纯文本即可）、退出后终端还原 |
| **C4.2** Transcript | ChatRenderer、事件 → 块（[12-ui §6.2](12-ui.md)）、各 View 的标题与主体、StreamReset、Ctrl+O | `ui/transcript.*` | C4.1 | 场景 13 |
| **C4.3** 输入 | PromptInput：发送、换行、排队、↑ 取回、斜杠命令；Keymap 命令 | `ui/prompt_input.*` | C4.1 | 场景 15 |
| **C4.4** 权限对话框 | ApprovalDialog + Approver（promise、stop_callback、只生效一次） | `ui/approval.*` | C4.1、C2.3 | 场景 14 |
| **C4.5** 活动行与状态栏 | Activity 各状态、计时器只在忙时运行、StatusLine | `ui/status_line.*`、`ui/shell.cpp` | C4.2 | 空闲时 `Runtime::wakeups()` 不再增长 |
| **C4.6** 信任与恢复 | 进入界面前的信任询问；`-r` / `--continue` 的历史重画 | `agent/main.cpp`、`ui/shell.cpp` | C4.2 | 场景 15 的恢复部分 |
| **C4.7** pty 脚本 | `temp/core_check/pty/`：启动 dagent、注入按键、读屏幕，复现场景 13–15 的关键路径 | `temp/core_check/pty/` | C4.2–C4.6 | 三个场景都能用脚本复现 |

### C4 验收场景

13. **显示**：在 bugproj 里跑场景 1 的任务。期望：Markdown 正文流式渲染；edit 显示 diff 且超过 20 行折叠；bash 输出实时出现、
    折叠到 10 行；让模型同时跑两个只读 bash，两个输出各在各的块里；拖选一段回复，粘贴出来内容正确。
14. **权限对话框**：让它依次做四件需要确认的事，分别按 `y`、`n`、`e`（输入「改用 cmake」）、`a`。期望：`n` 之后本轮结束并
    显示「已拒绝」；`e` 之后模型按说明换做法；`a` 之后同前缀的命令不再询问、不同前缀的仍询问。对话框打开时按 Ctrl+C：
    本轮中断，界面不卡。
15. **输入与恢复**：模型工作时输入第二条消息 → 出现在排队区 → 按 ↑ 取回修改 → 再 Enter → 本轮结束后自动发送；Esc 中断；
    空闲时 Ctrl+C 两次退出，打印恢复提示；`dagent -r <id>` 打开后历史和刚才实时看到的一致。

### C4 审核清单

- [ ] 控件树只在渲染线程上被碰；agent 线程只调 `post`
- [ ] Approver 在 stop 后 100 ms 内返回；回答只生效一次
- [ ] 空闲时零唤醒（计时器已取消）
- [ ] agent 线程最外层的异常会让界面退出而不是卡住
- [ ] 没有修改 `src/*/tui`

---

## 7. C5 上下文管理

目标：长任务不撞窗口，压缩后能正确恢复。

| 任务 | 内容 | 文件 | 依赖 | 完成标准 |
| --- | --- | --- | --- | --- |
| **C5.1** 预算与估算 | `Budget`、整请求估算、estimate / observe 对应、`ContextUpdate` 的 used / limit | `agent/compaction.*`、`agent/agent.cpp` | C1.8 | 状态栏 / jsonl 的上下文数字与 usage 吻合 |
| **C5.2** 第一级裁剪 | 保护区、占位（T11）、`prune` 记录与回放 | `agent/compaction.cpp`、`agent/record.cpp` | C5.1、C3.1 | 场景 16 |
| **C5.3** 第二级摘要 | 切点、摘要请求、T10、`compaction` 记录与回放、`compact.md` | `agent/compaction.cpp`、`prompts/compact.md` | C5.2 | 场景 17 |
| **C5.4** 强制与退化 | `context_too_long` → `force`；摘要失败时整批丢弃 | `agent/agent.cpp`、`compaction.cpp` | C5.3 | 场景 18 |
| **C5.5** 手动压缩 | `Agent::compact`、`/compact` | `agent/agent.cpp`、`ui/prompt_input.cpp` | C5.3、C4.3 | 交互界面里 `/compact` 后状态栏的上下文百分比下降 |
| **C5.6** 摘要质量 | 用场景 17 的摘要检查 [08-prompt §4.2](08-prompt.md) 的格式；不满足就改 `compact.md` | `prompts/compact.md` | C5.3 | 三次摘要都包含六项、用户请求是原文 |

### C5 验收场景

16. **裁剪**：`--set context.window_tokens=16000` 跑场景 1 的任务。期望：记录里出现 `prune`；任务仍然完成。
17. **摘要**：`--set context.window_tokens=8000` 跑一个需要读多个文件的任务。期望：记录里出现 `compaction`，摘要里有用户原始请求的
    原文；`-r` 恢复后，把恢复出的历史和压缩发生后内存里的历史逐条比较（role、content），完全一致。
18. **超长恢复**：`--set context.window_tokens=1000000 --set context.safety_margin_tokens=0`（远大于 DeepSeek 真实窗口），用一个会产生
    大量输出的任务把上下文撑到真实窗口以上。期望：服务端报超长后自动 `force` 压缩、重发成功；同一步第二次超长时以 failed
    结束并提示 `/new`。

### C5 审核清单

- [ ] 切点永远不在 assistant 和它的 tool 消息之间
- [ ] 压缩后 `validate()` 为空，I4（第一条是 user）成立
- [ ] observe 对应的是最后一次 estimate 的请求
- [ ] 摘要被取消时历史不变
- [ ] 回放用的占位和摘要包装文本与压缩时由同一个函数生成

---

## 8. C6 MCP

| 任务 | 内容 | 文件 | 依赖 | 完成标准 |
| --- | --- | --- | --- | --- |
| **C6.1** McpHub | 连接线程、`incoming`、`apply_pending`、状态机、析构顺序 | `agent/mcp_hub.*`、`agent/agent.cpp` | C1.8 | 场景 19 的连接部分、场景 20 |
| **C6.2** 刷新与重连 | `tools_changed`、`refresh_tools`、`mark_disconnected`、只重连一次 | `agent/mcp_hub.cpp`、`dispatch.cpp` | C6.1 | 场景 19 的重连部分 |
| **C6.3** 显示 | 状态栏 MCP 进度；run 模式 stderr 警告 | `ui/status_line.cpp`、`agent/headless.cpp` | C6.1、C4.5 | 场景 19、20 里状态显示正确 |

### C6 验收场景

19. **连接与重连**：用 `temp/tools_check/tools_server.py` 作 stdio server，启动前加 `sleep 5`。期望：dagent 启动后立即能输入；
    5 秒后下一步请求里出现它的工具，模型能调用；`kill` 掉 server 进程，下一次调用得到断连提示；再下一步前自动重连，调用成功。
20. **连不上**：再配置一个命令不存在的 server。期望：只有一条警告；其他工具（包括场景 19 的 server）正常；状态栏显示它 failed。

### C6 审核清单

- [ ] Client 析构之前它的工具已从 Registry 移除
- [ ] `on_tools_changed` 回调里只设标志
- [ ] Registry 只在 agent 线程、两次请求之间修改
- [ ] 析构时所有连接线程都 join

---

## 9. temp/core_check 布局

```
temp/core_check/
├── dagent.json            验收用配置：DeepSeek 网关、debug 日志、会话目录放在 temp 里
├── fault_proxy.py         故障注入代理（02-model §10）
├── bugproj/               场景 1：一个几百行的小 C++ 项目，有一个会让构建失败的 bug；build.sh
├── batchproj/             场景 5：六个独立的小源文件
├── reset.sh               把 bugproj / batchproj 恢复成初始状态（git stash 或从 .orig 复制）
├── c1.sh … c6.sh          每个里程碑的场景：准备 → 运行 dagent → 用 jq / grep 检查记录和输出，打印 PASS / FAIL
└── pty/                   C4 的 pty 驱动（Python pty 或 C++，参照 test/tui/runtime_test.cpp）
```

- 检查脚本只检查能自动判断的部分（退出码、记录序列、jq 断言）；模型行为是否合理（有没有先读后改、摘要质量）由人看输出判断。
- 模型输出有随机性：把 `temperature` 设成 0；一个场景失败时先重跑一次，两次都失败才算失败，并把两次的记录都留下来。
- 所有会话写到 `temp/core_check/sessions/`（配置里 `session.directory`），不污染用户的会话目录。

---

## 10. 风险

| 风险 | 影响 | 应对 |
| --- | --- | --- |
| 开发网关不支持工具调用 | 只能用 DeepSeek 验收，有费用和网络依赖 | C0.1 先确认；场景尽量短 |
| 模型不稳定地产生并行调用（场景 5） | C2.1 难以验收 | 提示词里鼓励并行；任务描述里明确要求；仍不行就把并行组的验证改为看日志里的一次偶然并行 |
| 提示词效果差 | C1 验收过不了 | C0.3 先用已有的最小循环调提示词，和代码开发并行 |
| 估算误差大 | 频繁触发 §7.4 的强制压缩 | 靠 `safety_margin_tokens` 兜底；真实使用中经常触发再考虑 tokenizer |
| 摘要质量差 | 压缩后模型忘事 | C5.6 专门迭代 `compact.md` |
| bash 能改受保护文件 | 安全缺口（[06-permission §9](06-permission.md)） | 第一版记为已知限制 |
