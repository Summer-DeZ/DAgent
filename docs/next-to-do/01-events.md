# 01 对外接口：Event、Sink、Approver

> 里程碑：C1（C2 补 Approver 的完整字段和 jsonl）· 头文件 `agent/events.hpp` · 实现 `events.cpp`

agent 和外界之间只有这一层接口：agent **发出**事件（`Sink`），在需要时**询问**权限（`Approver`），一轮结束时**返回**
`TurnStatus`。交互界面、run 模式、以后的任何前端都只实现这三样东西。

---

## 1. 职责

**做**：定义事件类型和发出顺序、权限询问的输入输出、一轮的结束状态、事件的 JSON 形式（run 模式 `--output jsonl`）。

**不做**：事件怎么显示（ui、headless 各自决定）；权限怎么判断（[06-permission](06-permission.md)）。

---

## 2. 接口

```cpp
namespace dagent::agent {

/// 一轮的结束状态。
enum class TurnStatus {
    done,        ///< 模型给出了不带工具调用的回复
    interrupted, ///< stop 被请求
    denied,      ///< 用户拒绝了某个调用（不带说明），停下来等新指示
    limit,       ///< 达到 max_model_calls 或 max_tool_calls
    failed,      ///< 模型不可用、上下文压不下来等，详见 TurnEnded::error
};

// ---- 事件 ----
struct TurnStarted   { std::string input; };
struct StepStarted   { int step; };                              ///< 第几次模型请求，从 1 开始
// TextDelta、ReasoningDelta：复用 llm.hpp 的类型
struct StreamReset   {};                                         ///< 这一步要重来：丢弃这一步已显示的内容
struct ToolPending   { std::string id, name; };                  ///< 流里出现了一个工具调用，参数还在传
struct ToolStarted   { std::string id, name, summary; tools::Grant grant; };
struct ToolOutput    { std::string id, chunk; };                 ///< bash 实时输出（原始块）
struct ToolFinished  { std::string id, name, summary; tools::Result result; };
struct Retrying      { int attempt, max_attempts; std::chrono::milliseconds wait; std::string reason; };
struct Compacted     { std::size_t before, after; bool summarized; }; ///< 估算 tokens，压缩前后
struct ContextUpdate { Usage usage; std::size_t used, limit; };  ///< 每步结束后
struct Notice        { enum class Level { info, warn, error } level; std::string text; };
struct TurnEnded     { TurnStatus status; std::string error; int steps, tool_calls; Usage total; };

using Event = std::variant<TurnStarted, StepStarted, TextDelta, ReasoningDelta, StreamReset, ToolPending,
                           ToolStarted, ToolOutput, ToolFinished, Retrying, Compacted, ContextUpdate,
                           Notice, TurnEnded>;
using Sink = std::function<void(const Event&)>;

// ---- 权限询问 ----
struct Approval {
    std::string call_id, tool;
    tools::Intent intent;        ///< 拷贝：交互界面要把它 post 到渲染线程
    std::string reason;          ///< 为什么要问，见 06-permission §4
    std::string session_rule;    ///< 选「本会话允许」会记住什么，给界面显示；为空表示不提供这个选项
    bool can_network = false;    ///< bash：是否提供「允许并联网」
};

struct Decision {
    enum class Answer { allow, allow_session, deny, deny_with_feedback };
    Answer answer = Answer::deny;
    std::string feedback;        ///< deny_with_feedback 时用户写的说明
    bool network = false;        ///< 只在 can_network 时有意义
};

using Approver = std::function<Decision(const Approval&, std::stop_token)>;

nlohmann::json to_json(const Event&);
std::string_view to_string(TurnStatus);  ///< "done" / "interrupted" / …

} // namespace dagent::agent
```

---

## 3. 事件顺序

一轮里的事件满足下面的文法（`*` 零到多次，`?` 可选，`|` 选一）：

```
Turn      := TurnStarted Step* TurnEnded
Step      := Compacted? StepStarted Attempt+ ContextUpdate? Batch?
Attempt   := (TextDelta | ReasoningDelta | ToolPending)* (Retrying StreamReset?)?
Batch     := CallEvents+                 并行组内各调用的事件可以交错
CallEvents:= ToolStarted ToolOutput* ToolFinished     执行了的调用
           | ToolFinished                             没执行的：参数错、未知工具、被拒、上限、中断
Notice 可以出现在 TurnStarted 之后、TurnEnded 之前的任何位置
```

保证：

1. 每轮恰好一个 `TurnStarted` 和一个 `TurnEnded`，`TurnEnded` 最后一个发出。
2. `StreamReset` 只紧跟在 `Retrying` 之后，而且只在这次尝试已经发出过 `TextDelta` / `ReasoningDelta` / `ToolPending`
   时才发。收到后，这一步（从最近的 `StepStarted` 起）已显示的内容全部作废。
3. 每个最终进入历史的 tool_call 都有且只有一个 `ToolFinished`；`ToolFinished` 按 tool_calls 的**原始顺序**发出
   （并行组跑完后统一按顺序发）。`ToolStarted` / `ToolOutput` 按实际发生的时间发出。
4. `ToolPending` 只是流式显示的提示：它对应的调用可能因为 `StreamReset` 或中断而作废，界面在 `StreamReset`、
   对应的 `ToolFinished`、`TurnEnded` 三者之一到来时清掉它。
5. 除 `ToolOutput` 外，所有事件都在 agent 线程上发出；`ToolOutput` 可能来自工具工作线程。同一个调用的
   `ToolOutput` 之间保持顺序。
6. 会话恢复时回放出来的历史也用这套事件（[09-record §5](09-record.md)），只是不含 `StepStarted`、`Retrying`、
   `StreamReset`、`ToolPending`、`ToolOutput`、`ContextUpdate`。

---

## 4. 各事件的来源

| 事件 | 在哪里发出 | 界面典型用法 |
| --- | --- | --- |
| `TurnStarted` | `run_turn` 开头，用户消息写入历史之后 | 画用户消息 |
| `StepStarted` | 每次模型请求之前（压缩之后） | Activity 显示「思考中」 |
| `TextDelta` / `ReasoningDelta` | Model 的 `on_event` | 流式 Markdown / 折叠的思考块 |
| `ToolPending` | Model 的 `on_event` 收到 `ToolCallBegin` | 「准备调用 edit…」 |
| `Retrying` | Model 的 `on_retry` | Notice「连接断开，2 秒后重试（1/2）」 |
| `ContextUpdate` | 一步结束、拿到 usage 后 | 状态栏的上下文百分比 |
| `ToolStarted` | 权限通过、`Call::run` 之前 | 工具块的标题行 |
| `ToolOutput` | `Call::run` 的 `on_output` | bash 输出块实时追加 |
| `ToolFinished` | 调用结果写入历史之后 | 工具块定稿：状态、diff、折叠 |
| `Compacted` | 压缩完成后 | Notice「上下文已压缩：180k → 90k」 |
| `Notice` | 各处 | Notice 条 / stderr |
| `TurnEnded` | 一轮收尾：历史闭合、`turn_end` 已落盘之后 | 恢复空闲状态、发送排队的输入 |

`summary` 字段：执行了的调用取 `Intent::summary`；prepare 失败或未知工具的调用没有 Intent，用
`<工具名> <参数的前 60 个字符>` 代替。

---

## 5. Approver 的约定

- 只在 agent 线程上调用，同一时刻至多一个。
- 调用期间 agent 线程阻塞；实现方负责在 stop 请求后 **100 ms 内**返回（返回什么都行，agent 会按中断处理，
  [04-turn §5](04-turn.md)）。
- 不能在 Approver 里调 `Agent` 的任何方法（agent 线程正卡在里面）。
- run 模式不会调用 Approver：`auto` / `deny` 模式下 Policy 自己就能决定（[06-permission §3](06-permission.md)）。
  run 模式传一个空的 `Approver{}`；Policy 在需要询问却没有 Approver 时按拒绝处理，并记一条 warn——这属于编程错误。

---

## 6. JSON 形式（`--output jsonl`）

每个事件一行，`type` 区分种类，字段名用 snake_case：

| 事件 | JSON |
| --- | --- |
| `TurnStarted` | `{"type":"turn_started","input":…}` |
| `StepStarted` | `{"type":"step_started","step":1}` |
| `TextDelta` | `{"type":"text","text":…}` |
| `ReasoningDelta` | `{"type":"reasoning","text":…}` |
| `StreamReset` | `{"type":"stream_reset"}` |
| `ToolPending` | `{"type":"tool_pending","id":…,"name":…}` |
| `ToolStarted` | `{"type":"tool_started","id":…,"name":…,"summary":…,"sandbox":"read_only","network":false}` |
| `ToolOutput` | `{"type":"tool_output","id":…,"chunk":…}` |
| `ToolFinished` | `{"type":"tool_finished","id":…,"name":…,"summary":…,"text":…,"is_error":…,"interrupted":…,"view":{…}}` |
| `Retrying` | `{"type":"retrying","attempt":1,"max_attempts":2,"wait_ms":2000,"reason":…}` |
| `Compacted` | `{"type":"compacted","before":…,"after":…,"summarized":true}` |
| `ContextUpdate` | `{"type":"context","prompt":…,"completion":…,"cached":…,"used":…,"limit":…}` |
| `Notice` | `{"type":"notice","level":"warn","text":…}` |
| `TurnEnded` | `{"type":"turn_ended","status":"done","error":"","steps":3,"tool_calls":7,"usage":{…}}` |

- `view` 用 `tools::to_json`。
- `ToolOutput.chunk` 是原始字节，先 `base::to_valid_utf8` 再放进 JSON。
- 流式 `TextDelta` 很碎；jsonl 不合并，保持一对一，调用方需要的话自己拼。

---

## 7. 实现要点

- `Event` 是值类型，`ToolFinished` 里有完整的 `tools::Result`（含 View，bash 的 output 可能有 256 KiB）。交互界面
  post 时按值捕获一次就够了，不要在 Sink 里再复制。
- 界面和 headless 都用 `std::visit` + overloaded lambda 处理，**不写 `auto` 兜底分支**：新加一种事件时，编译器会
  指出每个没处理的地方。
- `to_json` 放在 `events.cpp`，和类型定义在一起。

---

## 8. 验收

- C1：run 模式 text 输出能看到流式文本和每个工具的摘要行（plan 场景 1）。
- C2：`--output jsonl` 的每一行都能被 `jq` 解析；逐行检查满足 §3 的文法：一个 `turn_started` 开头，`turn_ended`
  结尾，每个 tool_call id 恰好一个 `tool_finished`（plan 场景 9）。
