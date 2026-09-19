# 09 会话记录与恢复

> 里程碑：C1 写入 · C3 回放重建、崩溃闭合、`--resume` / `--continue` · 头文件 `agent/record.hpp` · 实现 `record.cpp` · 依赖 session、tools（View 序列化）

session 模块只管信封和存取，`payload` 对它不透明（session 文档）。这里定义**核心往里写什么**，以及怎样从这些记录
重建出一模一样的历史。

---

## 1. 职责

**做**：定义记录类型和 payload；按正确顺序写入；写入失败时降级；从回放重建 Conversation 和界面事件；崩溃后闭合。

**不做**：存储格式、脱敏、blob、截断恢复（session 模块已做）。

---

## 2. 接口

```cpp
namespace dagent::agent {

class Recorder {
public:
    static Recorder create(const session::Options&, session::Meta);
    static Recorder resume(const session::Options&, std::string_view id);

    void system(std::string_view text);
    void user(std::int64_t n, std::string_view text);
    void assistant(std::int64_t n, const Reply&);
    void tool(std::int64_t n, const ToolCall&, std::string_view summary, const tools::Result&);
    void permission(std::string_view call_id, const Decision&, std::string_view rule);
    void prune(const std::vector<std::int64_t>& ordinals);
    void compaction(std::int64_t keep_from, std::string_view summary);
    void turn_end(TurnStatus, std::string_view error, int steps, int tool_calls, const Usage& total);
    void turn_end_crashed();
    void sync();

    const session::Meta& meta() const;
    bool broken() const;       ///< 写入失败过，之后的记录都被丢弃

private:
    void append(std::string_view type, nlohmann::json payload);  // catch SessionError → broken
};

struct Restored {
    Conversation conversation;
    bool unfinished;                      ///< 最后一轮没有 turn_end（崩溃或被杀）
    std::vector<ToolCall> open_calls;     ///< unfinished 时还没有结果的调用
};

/// 回放一个会话：重建历史，并把能画的历史事件交给 sink。
Restored replay_into(const session::Options&, std::string_view id, const Sink& sink);

/// session::list 的 title_of：第一条 user 记录的第一行，截到 60 个字符。
std::string session_title(const nlohmann::json& first_events);

} // namespace dagent::agent
```

---

## 3. 记录类型

`n` 是消息序号（ordinal），由 Conversation 分配（[03-conversation §3](03-conversation.md)），user / assistant / tool
三种记录共用一个递增序列。

| type | payload | 何时写 |
| --- | --- | --- |
| `system` | `{"schema":1,"text":…}` | 会话创建时，第一条核心记录 |
| `user` | `{"n":0,"text":…}` | 每轮开头 |
| `assistant` | `{"n":1,"content":…,"reasoning":…,"tool_calls":[{"id","name","arguments"}],"finish":"tool_calls","usage":{"prompt","completion","cached"}}` | 每一步拿到回复后 |
| `tool` | `{"n":2,"call_id":…,"name":…,"summary":…,"text":…,"is_error":false,"interrupted":false,"view":{…}}` | 调度器提交时（按原顺序） |
| `permission` | `{"call_id":…,"answer":"allow_session","rule":"npm test","network":false}` | 用户回答过的询问 |
| `prune` | `{"ordinals":[2,5,9]}` | 第一级裁剪 |
| `compaction` | `{"keep_from":40,"summary":…}` | 第二级摘要；`summary` 为空表示退化为丢弃 |
| `turn_end` | `{"status":"done","error":"","steps":3,"tool_calls":7,"usage":{…}}` | 每轮结束；`status` 还可能是 `crashed` |

- `schema` 是核心记录格式的版本。以后改格式时加一，恢复时遇到不认识的版本直接报错，不猜。
- `view` 用 `tools::to_json`；bash 的 output、read 等大字符串由 session 按 `max_inline_payload_bytes` 自动转 blob。
- `assistant` 的 `usage` 没有时省略。
- **payload 里不要用 `token`、`api_key`、`authorization` 做键名**：它们在 session 的默认脱敏列表里，会被替换掉。
  usage 的字段因此叫 `prompt` / `completion` / `cached`。

### 3.1 顺序

```
system
user(n0) assistant(n1) tool(n2) tool(n3) [permission…] assistant(n4) turn_end
user(n5) … [prune] … [compaction] … turn_end
```

- `permission` 写在对应的 `tool` 之前（先决定、后执行、再提交）。
- `prune` / `compaction` 写在它们发生的那一步的 `assistant` 之前。
- `turn_end` 之后 `sync()`。除此之外只在 Agent 析构时 `sync()`，不是每条都刷盘（session 文档：调用节奏由核心决定）。

---

## 4. 写入失败

磁盘满、目录被删时 `Writer::append` 抛 `SessionError{io}`：

1. Recorder 自己 catch，记 error 日志，设 `broken = true`；
2. Agent 在每次写入后看 `broken()`，第一次变成 true 时向当前轮的 Sink 发一次 `Notice(error)`「会话记录写入失败，之后的
   内容不会保存：…」（Recorder 自己不持有 Sink：Sink 是每轮传入的）；
3. 之后所有写入直接丢弃。

**不终止这一轮**：用户正在做的事比记录重要。这条 error Notice 会作为一个块留在对话里（[12-ui §6.2](12-ui.md)），用户不会错过。

---

## 5. 回放与重建（C3）

### 5.1 重建历史

```
replay_into(id, sink):
    entries = []; open_turn = false; max_n = -1
    session::replay(id, [&](type, p) {
        system:     检查 schema == 1，否则抛 SessionError{corrupt}「不认识的会话格式版本」
        user:       entries.push(user, n); open_turn = true;  sink(TurnStarted{text})
        assistant:  entries.push(assistant, n);                sink(ReasoningDelta{…}), sink(TextDelta{content})
        tool:       entries.push(tool, n, summary);            sink(ToolFinished{…, view_from_json(view)})
        prune:      对 ordinals 里的每个 tool entry：content ← placeholder(summary), pruned = true
        compaction: 摘要非空 → entries = [摘要 entry] + {e ∈ entries | e.ordinal ≥ keep_from}
                    摘要为空 → entries = [已有的摘要 entry（如果有）] + {e | e.ordinal ≥ keep_from}
        turn_end:   open_turn = false;                         sink(TurnEnded{…})
        permission: 忽略
        max_n = max(max_n, n)
    })
    conversation.restore(entries…); conversation.set_next_ordinal(max_n + 1)
    return {conversation, open_turn, conversation.open_calls()}
```

- 占位用和压缩时**同一个函数**生成（[07-context §4.2](07-context.md)），所以不需要把占位文本存下来。
- 摘要 entry 的 ordinal 是 −1；T10 的包装文本也由同一个函数生成，`compaction` 里只存模型写的摘要本身。
- 回放出来的事件只有 `TurnStarted`、`TextDelta`、`ReasoningDelta`、`ToolFinished`、`TurnEnded`（[01-events §3](01-events.md)
  保证 6），界面按同一套逻辑画历史。

### 5.2 崩溃闭合

`unfinished` 为 true（最后一轮没有 `turn_end`）时，`Agent::resume` 在 `Recorder::resume` 之后：

1. 对 `open_calls` 里每个调用：`conversation.add_tool_result(id, T9, …)`，`recorder.tool(…)`，发 `ToolFinished`；
2. `recorder.turn_end_crashed()`，发 `TurnEnded{failed, "会话意外中断"}`；
3. `sync()`。

这样 JSONL 自身也闭合了：再恢复一次，不会再补一遍。

| 崩溃时最后一条记录 | 闭合做什么 |
| --- | --- |
| `user` | 只补 `turn_end` |
| `assistant`，没有 tool_calls | 只补 `turn_end` |
| `assistant`，有 tool_calls | 每个调用补 T9，再补 `turn_end` |
| `tool`，同批还有调用没结果 | 剩下的补 T9，再补 `turn_end` |
| `permission` | 同上（对应的 tool 还没写） |

T9 说「结果未知，请先检查当前状态」而不是「没有执行」：崩溃时 edit 可能已经写完了，只是结果没来得及记录。

### 5.3 一致性检查

重建完成、闭合之后，`conversation.validate()` 必须为空。不为空说明记录本身有问题（手工改过、版本不一致），抛
`SessionError{corrupt}`，信息里带 validate 的描述，入口报错退出。**不要**尝试修复一份不一致的历史再发出去——
那只会得到一个 400，然后用户更困惑。

---

## 6. 恢复的其余步骤

`Agent::resume(setup, id, replay_sink)`（[04-turn §2](04-turn.md)）：

1. `replay_into` 重建；
2. `Recorder::resume(id)`（session 做尾部截断恢复）；
3. 需要时崩溃闭合（§5.2）；
4. 按**当前**环境重新渲染 system prompt，写一条新的 `system` 记录；
5. FileTracker 从空开始（tools 文档的要求：宁可让模型多读一次，也不能拿旧 Stamp 覆盖别人的改动）；
6. 会话授权从空开始（[06-permission §6](06-permission.md)）；
7. meta 里的 `model` 和当前配置不同时，发 `Notice(info)`「这个会话原来用的是 X，现在用 Y 继续」。

### 找会话

| 参数 | 做法 |
| --- | --- |
| `--resume <id>` | 直接用；也接受 id 的唯一前缀（在这个项目全部会话的 `list` 结果里匹配，不唯一时报错并列出候选） |
| `--continue` | `session::list(project_root, 1, session_title)` 的第一个；没有就报错「这个项目还没有会话」 |
| `dagent sessions` | `list(project_root, 20, session_title)`，格式见 [11-entry §5](11-entry.md) |

`session.record_payloads = false` 时记录里只有信封，无法恢复：`replay_into` 发现 `system` 记录没有 payload 时报错
「这个会话创建时关闭了 record_payloads，无法恢复」。

---

## 7. 验收

- C1：plan 场景 2——`jq -c '{type, n: .payload.n}' events.jsonl` 能看到完整序列；`grep` 不到 api key。
- C3：plan 场景 10（中断后恢复）、11（`kill -9` 后恢复并闭合，再恢复一次不会重复闭合）、12（sessions、`--continue`、id 前缀）。
- C5：plan 场景 17 的恢复部分——恢复前后逐条比较历史的 role 和 content 完全一致。
