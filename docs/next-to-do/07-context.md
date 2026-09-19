# 07 上下文管理

> 里程碑：C5 · 头文件 `agent/compaction.hpp` · 实现 `compaction.cpp` · 依赖 Conversation、Model、TokenEstimator

长任务的历史迟早会超过模型窗口。这里决定**什么时候**压缩、**先压什么**、**怎样压**，以及压缩后怎样记录才能在
恢复时重建出同样的历史。

C5 之前（C1–C4）只做一件事：服务端报上下文超长时以 `failed` 结束，提示用户 `/new`。

---

## 1. 职责

**做**：预算计算；第一级裁剪（不调模型）；第二级摘要（调一次模型）；服务端报超长时的强制压缩；手动 `/compact`；
压缩记录。

**不做**：清 FileTracker（§6）；改 system prompt。

---

## 2. 接口

```cpp
namespace dagent::agent {

struct ContextOptions {                        ///< config "context" 段，从 app 迁来
    std::size_t window_tokens = 262144;
    std::size_t safety_margin_tokens = 8192;
    int compaction_trigger_percent = 80;
    int compaction_target_percent = 60;
};

struct Budget {
    std::size_t limit, trigger, target;
    static Budget from(const ContextOptions&, std::size_t max_tokens);
};

class Compactor {
public:
    Compactor(ContextOptions, std::size_t max_tokens, std::string compact_prompt);

    /// 每一步请求前调用。估算超过 trigger 才动手。可能调一次 Model；stop 时抛 ModelError{cancelled}，历史不变。
    void maybe_compact(Conversation&, const RequestShape&, Model&, TokenEstimator&, Recorder&, const Sink&,
                       std::stop_token);

    /// 服务端报超长：不看 trigger，先裁剪保护区以外的全部工具输出，再做摘要。
    void force(…同上…);

    /// /compact：直接做摘要。
    void summarize(…同上…);

    Budget budget() const;
};

} // namespace dagent::agent
```

`RequestShape` 是 build Request 需要的其余部分（system prompt、工具定义、ModelParams），用来做**整请求**的估算——
system prompt 和工具定义本身就可能有上万 token，只估历史会系统性偏低。

---

## 3. 预算

```
limit   = window_tokens − safety_margin_tokens − max_tokens
trigger = limit × compaction_trigger_percent / 100
target  = limit × compaction_target_percent / 100
```

默认值下：`limit = 262144 − 8192 − 4096 = 249856`，`trigger ≈ 199884`，`target ≈ 149913`。

### 3.1 估算

- 每一步：`request = conversation.build(shape)`，`est = estimator.estimate(request)`；超过 `trigger` → 压缩 → 重新 build
  并估算。发出请求、拿到 usage 后 `estimator.observe_prompt_tokens(usage.prompt)`。
- **`observe` 必须对应最后一次 `estimate` 的那个请求**（TokenEstimator 用上一次估算值算校正比例）。压缩后重新
  estimate，保证对上。
- 状态栏的「上下文已用」：有 usage 时用 `usage.prompt + usage.completion`，没有时用估算值；分母是 `limit`。

---

## 4. 两级压缩

### 4.1 保护区

从历史末尾往前累加 `entry.tokens`，直到累计超过 `target × 25%`，再往前扩展到包含**最近一批**的全部 tool 消息。保护区
里的内容两级压缩都不动：模型正在用它。

### 4.2 第一级：裁剪旧工具输出（不调模型）

```
for i in 0 ..< 保护区起点:
    e = entries[i]
    if e 是 tool 消息 and not e.pruned and e.tokens > 256:
        conversation.prune(i, placeholder(e))
        if 估算 ≤ target: break
```

占位（T11）用这条 tool 消息的 `summary`（调度时的 `Intent::summary`，[03-conversation §3](03-conversation.md) 的
`Entry::summary`）：

```
[旧的工具输出已省略：读取 src/a.cpp 1–200 行。需要时请重新调用。]
[旧的工具输出已省略：运行 make test（退出码 1）。需要时请重新调用。]
```

- 只改 content，不删消息，闭合不受影响。
- 裁剪是**确定性**的：占位只由 summary 决定，恢复时按记录里的序号重新生成，不需要存占位文本。
- 大多数长会话靠这一级就够了：上下文的大头是 read 和 bash 的输出，而模型通常早已不需要原文。

### 4.3 第二级：摘要（调一次模型）

第一级之后估算仍然超过 `target` 时：

**① 选切点**

```
cuts = conversation.safe_cuts()          # entries[i] 是 user 或 assistant 的下标 i（i ≥ 1）
c = 满足「tokens(entries[c..]) ≤ target × 50%」的最小 cut      # 尾部尽量长
若不存在：c = 最大的 cut                   # 最近一批本身就很大
若 cuts 为空：无法摘要 → 退化（§4.5）
```

- 安全切点永远不在 assistant 和它的 tool 消息之间，所以切开后两边都是闭合的。
- **切点可以落在当前轮中间**：一轮里调了上百次工具时，只能在批与批之间切。尾部以 assistant 开头也合法（前面会是
  摘要这条 user 消息）。

**② 摘要请求**

```
prefix = entries[0 ..< c]，其中所有 tool 消息都换成占位（比第一级更彻底）
request.messages = [system: compact.md] + prefix + [user: 「请按要求总结以上对话。」]
request.tools = []                        # 不带工具
request.max_tokens = gateway 的 max_tokens
```

- prefix 仍然超过 `limit` 时，从最前面整批丢弃，直到放得下，并在最后那条 user 里注明「更早的对话已丢弃」。
- 走同一个 `Model::complete`，同样有重试；`on_event` 不外发（摘要过程不显示给用户），只在完成后发 `Compacted`。

**③ 替换**（T10）

```
<summary>
{模型的摘要}
</summary>
以上是之前对话的摘要，原始消息已被压缩。请继续完成用户的任务；需要文件内容时重新读取，不要凭摘要猜测文件内容。
```

`conversation.replace_prefix(c, 上面的文本)`：删掉 `entries[0 ..< c]`，在最前面放一条 ordinal = −1 的 user 消息。上一次的
摘要（如果有）在 prefix 里，会被一起总结进新摘要。

**④ 记录与事件**：`recorder.compaction(summary, keep_from = entries[1].ordinal)`（即切点处那条消息的序号），
`sink(Compacted{before, after, summarized = true})`。

### 4.4 摘要提示词要点

`prompts/compact.md` 要求摘要**只**包含（[08-prompt §4](08-prompt.md)）：

1. 用户的原始请求，**原文照抄**；以及之后的每一次补充和修正；
2. 已经做出的决定和理由；
3. 改过的文件，每个一行：路径 + 改了什么；
4. 当前进行到哪一步；
5. 还没做的事；
6. 遇到过、还没解决的报错（原文）。

不要客套，不要复述工具输出的原文。

### 4.5 失败与退化

| 情况 | 处理 |
| --- | --- |
| 摘要请求 `rejected` / `exhausted` / `context_too_long` | 退化：从最前面**整批**丢弃（保留摘要消息），直到估算 ≤ target；`Notice(warn)`「摘要失败，已丢弃最早的 N 条消息」；记录为 `compaction`，summary 为空 |
| 摘要请求被取消 | 历史不变，抛 `cancelled`，本轮以 interrupted 结束 |
| 摘要比 prefix 还长 | 照样替换（极少见）；如果仍超过 trigger，下一步会再压一次 |
| 没有可用切点（历史只有当前这一批） | 只能靠第一级；仍然超长就以 `failed` 结束，提示 `/new` |

---

## 5. 触发点

| 触发 | 做什么 |
| --- | --- |
| 每一步请求前，估算 > trigger | 第一级，不够再第二级 |
| 服务端报 `context_too_long`（估算偏低） | `force`：第一级裁掉保护区以外的全部，再第二级；然后重发这一步。同一步第二次超长 → `failed` |
| `/compact` | 第二级 |

---

## 6. 与其他部件的关系

- **FileTracker 不清**：模型忘了文件内容却还有 Stamp，最坏是 edit 的 `old_string` 匹配不上而报错，不会写坏文件；
  清掉它只会逼模型多读一次。摘要消息里已经提醒「不要凭摘要猜测文件内容」。
- **prompt 缓存**：压缩会让缓存失效一次，之后又是只追加。所以压缩要「一次压到 target」，而不是每步压一点。
- **恢复**：`prune` 和 `compaction` 两种记录足够重建（[09-record §5](09-record.md)）。

---

## 7. 验收

- plan 场景 16：`window_tokens` 调到 16k 跑场景 1 的任务——发生第一级裁剪，任务仍然完成。
- plan 场景 17：调到 8k——发生摘要，摘要里有用户原始请求的原文；`--resume` 之后的历史与恢复前一致（逐条比较
  `role` 和 content）。
- plan 场景 18：`safety_margin_tokens` 设成 0、`window_tokens` 设得比模型真实窗口大——服务端报超长后自动 `force` 并重发成功。
- `/compact` 在交互界面里可用，状态栏的上下文百分比随之下降。
