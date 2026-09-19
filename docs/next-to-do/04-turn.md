# 04 Agent 与一轮循环

> 里程碑：C1（C2 补上限的收尾步、C5 补压缩的接入）· 头文件 `agent/agent.hpp` · 实现 `agent.cpp`

`Agent` 是一个会话的全部状态加上 `run_turn`：把用户输入放进历史，反复「请求模型 → 执行工具 → 回填结果」，
直到模型不再调用工具、用户中断、被拒绝、达到上限或出错。

---

## 1. 职责

**做**：持有并按正确顺序构造 / 析构一个会话的所有部件；一轮的循环、计数和收尾；把 Model 的流和调度器的结果转成
对外事件；保证每一种退出路径之后历史闭合、记录完整。

**不做**：线程管理（调用方在哪个线程调它，它就在哪个线程跑）；任何 UI。

---

## 2. 接口

```cpp
namespace dagent::agent {

class Agent {
public:
    /// 新会话：渲染 system prompt、创建会话记录、注册内置工具、启动 MCP 连接。失败时抛外围异常。
    static std::unique_ptr<Agent> create(Setup setup);

    /// 恢复会话：回放记录重建历史（09-record），回放出来的事件交给 replay。
    static std::unique_ptr<Agent> resume(Setup setup, std::string_view session_id, const Sink& replay);

    ~Agent();   // 停止 MCP 连接线程、sync 会话记录

    /// 一轮。阻塞到结束；不抛异常（除编程错误）。
    TurnStatus run_turn(std::string input, const Sink& sink, const Approver& approver, std::stop_token stop);

    /// 手动压缩（/compact）。
    void compact(const Sink& sink, std::stop_token stop);

    /// 权限模式（交互界面的 Shift+Tab）。线程安全：内部是 atomic，下一次决策生效。
    void set_permission_mode(PermissionMode mode);

    const session::Meta& meta() const;

private:
    // 成员声明顺序即构造顺序，析构倒序——见 §3
};

} // namespace dagent::agent
```

`Setup` 定义在 `agent/options.hpp`，内容和装配方式见 [11-entry §2](11-entry.md)。Agent 不读任何配置文件。

---

## 3. 组成与构造顺序

```cpp
Setup setup_;
Recorder recorder_;            // 09-record
McpHub hub_;                   // 10-mcp：必须比 registry_ 活得久（MCP 工具引用着 Client）
tools::Registry registry_;
tools::Context tool_ctx_;      // 工作区根、各 Options、FileTracker
Policy policy_;                // 06-permission
Model model_;                  // 02-model
Conversation conversation_;    // 03-conversation
TokenEstimator estimator_;     // llm.hpp
Compactor compactor_;          // 07-context
std::string system_prompt_;    // 08-prompt，会话开始时渲染一次
```

**顺序是有意义的**：析构倒序进行，`registry_` 先于 `hub_` 析构，工具不会悬空引用已经销毁的 `mcp::Client`。
调整成员顺序时要保持这一点，并在注释里写明。

`create` 的步骤：

1. `workspace::collect_environment(setup.cwd)` → 渲染 system prompt（模板出错直接抛，启动失败，[08-prompt §5](08-prompt.md)）。
2. `Recorder::create`（写 meta 行）。
3. `tools::add_builtin(registry_)`。
4. `McpHub` 为每个 server 起连接线程，不等待。
5. `Policy` 用 `setup.sandbox`（`exec::probe()` 的结果）和权限模式初始化。

---

## 4. 一轮的循环

```
run_turn(input, sink, approver, stop):
    input = to_valid_utf8(input)
    recorder.user(conversation.add_user(input), input)
    sink(TurnStarted{input})
    steps = 0; calls = 0; forced_compaction = false; grace = false; total = {}

    loop:
        if steps == max_model_calls:          → finish(limit, 「本轮模型调用次数已达上限」)
        hub.apply_pending(registry, sink)                                 # 10-mcp
        compactor.maybe_compact(conversation, sink, stop)                 # 07-context，可能调一次 Model
        steps += 1; sink(StepStarted{steps})
        try:
            reply = model.complete(conversation.build(...), on_stream, on_retry, stop)
        catch ModelError e:
            cancelled        → keep_partial(e.partial()); finish(interrupted)
            context_too_long → if forced_compaction: finish(failed, 「上下文超出模型窗口，压缩后仍然超长；请用 /new 开始新会话」)
                               compactor.force(conversation, sink, stop); forced_compaction = true
                               steps -= 1; continue            # 这次不计数
            rejected / exhausted → finish(failed, e.what())

        if reply 为空（无文本、无调用）: sink(Notice(warn, 「模型返回了空回复」)); finish(done)
        recorder.assistant(conversation.add_assistant(reply.message), reply)
        estimator.observe(usage); total += usage; sink(ContextUpdate{…})
        if reply.message.tool_calls 为空: 按 §5 提示; finish(done)

        outcome = dispatch(reply.message.tool_calls, max_tool_calls - calls, …)   # 05-dispatch
        calls += outcome.handled
        switch outcome.stop:
            none        → if grace: finish(limit)            # 收尾步里模型还在调工具
                          if outcome.hit_limit: grace = true  # 给一次不计数的收尾机会
                          continue
            interrupted → finish(interrupted)
            denied      → finish(denied)

finish(status, error = ""):
    若历史仍打开：每个 open_calls() 补 T2，并记录（防御最后一道；正常情况下调度器已闭合）
    recorder.turn_end(status, error, steps, calls, total); recorder.sync()
    sink(TurnEnded{status, error, steps, calls, total})
    return status
```

- **是否执行工具只看 tool_calls 是否为空**，不看 `finish_reason`：有的网关带着工具调用却报 `stop`。
- `on_stream`：把 StreamEvent 转成对外事件（[02-model §3.2](02-model.md)）。
- `on_retry`：发 `Retrying`；`had_output` 时再发 `StreamReset`。
- 收尾步（`grace`）：工具调用超额的那一批，超出的调用拿到 T8；再请求一次模型让它总结。这一步如果模型**还**调工具，
  这些调用同样全部拿到 T8，然后以 `limit` 结束。收尾步计入 `steps`，但 `steps == max_model_calls` 时不再给收尾步。

---

## 5. 结束原因

| 模型回复 | 处理 | 状态 |
| --- | --- | --- |
| 有 tool_calls（无论 finish_reason） | 执行，继续循环 | — |
| `stop` | 结束 | done |
| `length`，没有 tool_calls | `Notice(warn)`：「回复达到 max_tokens 上限被截断，可以输入“继续”」 | done |
| `length`，有 tool_calls | 照常执行；被截断的那个调用参数是坏 JSON，prepare 报错，模型看得到并重来 | — |
| `content_filter` | `Notice(warn)`：「回复被服务端的内容过滤截断」 | done |
| 空回复 | 不加入历史，`Notice(warn)` | done |

`length` 不自动续写：续写需要把半截回复当作 assistant 前缀发回去，各网关行为不一，而且用户输入「继续」就能达到同样效果。

---

## 6. 中断

用户按 Esc（交互）或 Ctrl+C（run 模式）都是 `request_stop()`。stop 可能在任何一个阻塞点生效：

| 生效时在做什么 | 历史里留下什么 | 记录 |
| --- | --- | --- |
| 压缩的摘要请求 | 历史不变（压缩放弃） | 无 |
| 模型请求，第一个可见 token 之前 | 什么都不加 | 无 |
| 模型请求，已经有文本 | assistant：已收到的文本 + T1；丢弃不完整的 tool_calls 和 reasoning | `assistant` |
| 重试等待中 | 什么都不加（失败的那次尝试已经作废） | 无 |
| 等待权限回答 | 这个调用 T2；同批剩余调用 T2 | `tool` × N |
| 并行组 / 串行工具执行中 | 正在跑的调用：`interrupted` 结果（text 里是已有的部分输出）；没开始的：T2 | `tool` × N |

中断后一律以 `interrupted` 结束，不自动继续；下一轮用户输入时历史已经闭合，可以直接请求。

---

## 7. 错误处理

| 来源 | 处理 |
| --- | --- |
| `ModelError` | 见 §4 |
| `Recorder` 写入失败（`SessionError::io`） | Recorder 自己 catch 并进入停用状态；Agent 看到 `broken()` 第一次变真时发 `Notice(error)`，本轮继续（[09-record §4](09-record.md)） |
| `McpError`（合并、刷新时） | McpHub 自己 catch，发 `Notice(warn)`，不影响本轮 |
| tools 层 | `Call::run` 不抛异常 |
| 其他异常 | 编程错误，不 catch，让它传出去（交互界面会在 agent 线程的最外层记日志并退出） |

---

## 8. 后置条件

`run_turn` 返回时，**无论哪条路径**，都必须满足：

1. `conversation.validate()` 为空（历史闭合）。
2. 最后一条记录是 `turn_end`，并且已经 `sync`。
3. 最后一个事件是 `TurnEnded`，状态和返回值一致。
4. 没有还在运行的工具工作线程（并行组的 jthread 都已 join）。
5. stop 被请求过的话，返回值是 `interrupted`（`failed` 除外：出错与中断同时发生时，以先发生的为准）。

实现时把收尾集中在一个 `finish` 里，所有退出点都走它；不要在循环里散落 `sink(TurnEnded)`。

---

## 9. compact()

`/compact` 调用。直接做第二级摘要（[07-context §4](07-context.md)），不看是否超过触发线；历史太短（切点之前没有
内容）时发 `Notice(info)`「没有可压缩的旧历史」。不产生 `TurnStarted` / `TurnEnded`，发 `Compacted`、`ContextUpdate` 和
`Notice`；返回 `TurnStatus`，由调用方结束忙碌状态。记录在操作完成后同步，取消不提交历史修改。

---

## 10. 验收

- C1：plan 场景 1（完整任务）、2（记录完整）。
- C2：场景 8（中断的每一行都成立）。
- C2：把 `run.max_tool_calls` 设成 3 跑场景 1：第 4 个调用起拿到 T8，模型在收尾步里给出总结，以 `limit` 结束，
  退出码 1（plan 场景 9 的一部分）。
