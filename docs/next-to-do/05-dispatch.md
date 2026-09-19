# 05 工具调度

> 里程碑：C1 串行版 · C2 并行组、上限、完整的权限分支 · 实现 `dispatch.cpp`（Agent 的私有部分，无公开头文件）

输入是一条 assistant 消息的 tool_calls（一批），输出是按原顺序追加进历史的 tool 消息。tools 层已经保证 `prepare`
无副作用、`run` 不抛异常；调度器只决定**顺序、权限和并行**。

---

## 1. 职责

**做**：逐个 prepare → 权限决策 → 执行；把可以并行的只读调用放进并行组一起跑；在任何情况下都让这一批闭合；
按顺序发事件、写记录。

**不做**：判断权限（交给 Policy，[06-permission](06-permission.md)）；工具语义（tools 层）。

---

## 2. 接口（Agent 内部）

```cpp
struct DispatchOutcome {
    enum class Stop { none, interrupted, denied } stop = Stop::none;
    int handled = 0;        ///< 这一批里计入 max_tool_calls 的调用数
    bool hit_limit = false; ///< 有调用因为超额拿到了 T8
};

DispatchOutcome Agent::dispatch(const std::vector<ToolCall>& calls, int budget,
                                const Sink&, const Approver&, std::stop_token);
```

`budget` = `max_tool_calls − 本轮已处理的调用数`。

---

## 3. 规则

1. **可并行**的调用 = Policy 直接放行（不需要询问），并且 Intent 是 `read`，或者是 `exec` 且 Grant 是 `read_only` 沙箱
   （即 `known_readonly` 的 bash）。其余一律串行：write、edit、MCP、需要询问的调用。
2. **prepare 紧跟在前一个调用执行之后做**，不一开始就把整批 prepare 好：
   - 「edit a → edit a」：第二个 edit 必须基于第一个写完的内容算 diff；
   - 「read a → edit a」：edit 要求 FileTracker 里已经有 a，而 read 可能还在并行组里没跑。
3. **遇到不能加入并行组的调用时，先把挂起的并行组跑完，再重新 prepare 这个调用**。prepare 没有副作用，重做一次只是
   多读一次文件，换来核心里不需要硬编码「哪个工具依赖哪个工具」。
4. **需要询问的调用，询问之前先把挂起的并行组跑完**（由规则 3 自然保证）：对话框出现时，前面的只读调用已经有结果，
   界面上的顺序和执行顺序一致。
5. 结果**按 tool_calls 的原始顺序**写入历史、写记录、发 `ToolFinished`，和实际完成顺序无关；能提交的尽早提交（§4.3）。
6. 任何一条退出路径都让这一批闭合：没执行的调用也拿到一条标准文本（[03-conversation §5](03-conversation.md)）。

---

## 4. 算法

### 4.1 数据

```cpp
struct Slot {
    const ToolCall* call;
    std::string summary;                 // ToolFinished 用
    std::optional<tools::Result> result; // 有值 = 可以提交
};
std::vector<Slot> slots;                 // 与 calls 一一对应
std::size_t committed = 0;               // 已经提交到历史的前缀长度

struct Pending { std::size_t slot; std::unique_ptr<tools::Call> call; tools::Grant grant; };
std::vector<Pending> group;              // 挂起的并行组
Stop stop_reason = none;
```

### 4.2 主循环

```
for i in 0 ..< calls.size():
    s = slots[i]; s.summary = 「<name> <arguments 前 60 个字符>」   # prepare 成功后换成 Intent::summary
    if stop_reason == interrupted or stop.stop_requested():
        stop_reason = interrupted; s.result = T2; continue
    if stop_reason == denied:            s.result = T5; continue
    if handled == budget:                s.result = T8; hit_limit = true; continue
    handled += 1

    tool = registry.find(call.name)
    if !tool:                            s.result = T7(name, 可用工具名); commit(); continue

    prepared = tool.prepare(call.arguments, ctx)
    verdict  = prepared ? policy.evaluate(call, intent) : —
    if group 非空 and not (prepared and parallel(verdict)):
        run_group()                                        # §4.4
        prepared = tool.prepare(call.arguments, ctx)       # 规则 3：重新 prepare
        verdict  = prepared ? policy.evaluate(call, intent) : —
    if !prepared:                        s.result = prepared.error(); commit(); continue
    s.summary = intent.summary

    switch verdict:
      allow(grant):
        if parallel(verdict): group.push({i, call, grant})
        else:                 run_serial(i, call, grant)
      deny(reason):                                        # 策略拒绝（run 模式、受保护文件）
        s.result = T6(reason); record permission
      ask(approval):
        decision = approver(approval, stop)                # 阻塞，06-permission §5
        if stop.stop_requested(): s.result = T2; stop_reason = interrupted; continue
        record permission
        switch decision.answer:
          allow / allow_session: policy.remember(...); run_serial(i, call, policy.grant(decision))
          deny:                  s.result = T3; stop_reason = denied
          deny_with_feedback:    s.result = T4(feedback)
    commit()

run_group()
commit()
return {stop_reason, handled, hit_limit}
```

`run_serial(i, call, grant)`：`sink(ToolStarted)` → `call.run(grant, on_output(id), stop)`（在 agent 线程上）→ 存进
`slots[i].result`。

### 4.3 提交

```
commit():
    while committed < slots.size() and slots[committed].result 有值:
        s = slots[committed]
        if s.result.display 是 McpView 且 disconnected:
            s.result.text += hub.mark_disconnected(server, s.result.text)   # T12 / T13（10-mcp §3）
        ordinal = conversation.add_tool_result(s.call.id, s.result.text, s.summary)
        recorder.tool(ordinal, s.call, s.result)
        sink(ToolFinished{s.call.id, s.call.name, s.summary, s.result})
        committed += 1
```

「尽早提交」让串行调用一结束就落盘：进程如果在后面某个调用执行时崩溃，前面已经完成的调用结果不会丢，恢复时只有
真正没结果的调用才拿到「结果未知」（T9）。

### 4.4 并行组

```
run_group():
    for p in group: sink(ToolStarted{…})            # 在 agent 线程上，按顺序
    分块，每块至多 8 个：
        每个调用一个 std::jthread：slots[p.slot].result = p.call.run(p.grant, on_output(id), stop)
        join 这一块
    group.clear()
```

- 每个工作线程只写自己那个 slot，join 之后 agent 线程再读，不需要额外的锁。
- 所有工作线程共用同一个 stop_token；stop 时它们各自返回 `interrupted`，join 仍然很快。
- `on_output` 在工作线程上调 Sink（`ToolOutput`），这就是「Sink 必须线程安全」的原因。
- FileTracker 内部有锁（tools 文档），并行的 read 可以同时更新它。
- 分块而不是线程池：一批里的调用很少超过十几个，没必要为它维护常驻线程。

---

## 5. 边界情况

| 情况 | 结果 |
| --- | --- |
| read a、read b、read c | 同一个并行组 |
| read a → edit a | read 进组；edit 不可并行 → 先跑组 → 重新 prepare edit，成功 |
| edit a → edit a | 第一个串行执行完才 prepare 第二个，第二个基于新内容 |
| read a → read a | 同组并行，两个结果都正确 |
| grep → 需要询问的 bash → read | grep 进组；bash 触发跑组、再询问；read 在 bash 之后另起一组 |
| `ls && make` 这类非只读 bash | 询问，串行 |
| 同一批两个 `known_readonly` bash | 并行，各自的 `ToolOutput` 按 id 区分 |
| 第 2 个调用被拒（deny） | 第 2 个 T3，第 3 个起 T5，本批结束后本轮以 denied 结束 |
| 第 2 个调用被拒并说明 | 第 2 个 T4，后面的照常处理，本轮继续 |
| run 模式下写受保护文件 | T6，后面的照常处理——策略拒绝不结束本轮，模型会换做法或在回复里说明 |
| 并行组执行中 stop | 组内都返回 interrupted；组之后的调用 T2 |
| 等待权限时 stop | 这个调用 T2，之后的 T2 |
| 未知工具 | T7，不触发跑组（没有 prepare） |
| 预算只剩 2，这批有 5 个调用 | 前 2 个照常，后 3 个 T8，`hit_limit = true` |
| 参数是坏 JSON | prepare 返回 is_error 的 Result，直接作为结果 |

---

## 6. 实现要点

- 串行和并行的执行都走同一个「执行一个调用并把结果放进 slot」的函数，差别只在哪个线程上调它。
- 重新 prepare 时，旧的 prepared 直接丢弃（`unique_ptr` 析构），不要复用旧 Intent。
- `summary` 以最后一次 prepare 的 Intent 为准。
- 在 debug 构建里，`dispatch` 返回前 `assert(committed == slots.size())`。
- 结果的 View 是 `McpView` 且 `disconnected` 时，调 `hub.mark_disconnected(server, …)`（C6，[10-mcp §5](10-mcp.md)）。

---

## 7. 验收

- C1：串行版跑通 plan 场景 1。
- C2：plan 场景 5——一次回复里 4 个 read + 1 个只读 bash，日志时间戳显示它们重叠执行；「read a → edit a」和
  「edit a → edit a」在同一批里都成功。让模型稳定地产生这种批次的办法：在 `temp/core_check` 的任务提示里明确要求
  「一次性并行读取这几个文件」「在同一次回复里连续做两处修改」。
