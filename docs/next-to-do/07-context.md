# 07 上下文管理

> 里程碑：C5 · 头文件 `agent/compaction.hpp` · 实现 `compaction.cpp` · 依赖 Conversation、Model、TokenEstimator

长任务的历史迟早会超过模型窗口。这里决定**什么时候**压缩、**先压什么**、**怎样压**，以及压缩后怎样记录才能在
恢复时重建出同样的历史。

C5 已实现：自动裁剪与摘要、服务端超长后的单次强制压缩、手动 `/compact`，以及压缩记录的回放。

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

预留量不小于窗口时 `limit` 为零，不做无符号减法回绕或退回整个窗口；无法继续模型请求。
小窗口验收须同时缩小预留量，例如 `window_tokens=8000`、`safety_margin_tokens=512`、`gateway.max_tokens=2048`。
百分比计算按 0–100 限定。

### 3.1 估算

- 每一步：`request = conversation.build(shape)`，`est = estimator.estimate(request)`；超过 `trigger` → 压缩 → 重新 build
  并估算。发出请求、拿到 usage 后 `estimator.observe_prompt_tokens(usage.prompt)`。
- **`observe` 必须对应最后一次 `estimate` 的那个请求**（TokenEstimator 用上一次估算值算校正比例）。压缩后重新
  estimate，保证对上。
- 状态栏的「上下文已用」：有 usage 时用 `usage.prompt + usage.completion`，没有时用估算值；分母是 `limit`。
- 请求前、压缩后和恢复会话时也发 `ContextUpdate`，使用整请求估算。摘要请求独立 estimate / observe，主请求重建后再 estimate。

---

## 4. 两级压缩

### 4.1 保护区

从历史末尾往前累加 `entry.tokens`，直到累计超过 `target × 25%`，再往前扩展到包含**最近一批**的全部 tool 消息。保护区
里的内容两级压缩都不动：模型正在用它。

保护区向前对齐到安全切点，并始终包括最近一个 assistant 工具调用批（即使后面已有自然语言回复）。
强制压缩和手动压缩的保留目标使用 `min(target, 历史缓存 tokens / 2)`：服务端已经报超长时不能继续依赖配置的过大窗口；
手动命令也应能压缩尚未达到自动阈值的历史。system 和工具定义仍计入整请求预算。

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
- 仅靠提示词不能保证模型及时记录结论。若待裁剪的工具结果之后还没有 assistant 正文，即使裁剪后已低于 target，
  也让第二级摘要覆盖这些结果，先从原文提取事实再提交裁剪。切点至少越过这些结果，仍不进入保护区；
  已有后续正文的历史仍可以只做第一级，不增加模型调用。

### 4.3 第二级：摘要（调一次模型）

第一级之后估算仍然超过 `target`，或待裁剪结果尚无后续 assistant 正文时：

**① 选切点**

```
cuts = conversation.safe_cuts()          # 前缀至少含一条 assistant，并覆盖尚无后续正文的待裁剪结果
c = 保护区起点以内、满足「tokens(entries[c..]) ≤ target × 50%」的最小 cut  # 尾部尽量长
若不存在：c = 保护区起点以内的最大 cut       # 最近一批本身就很大，也不破坏保护区
若 cuts 为空：没有可压缩的旧历史，不调用摘要模型
```

- 安全切点永远不在 assistant 和它的 tool 消息之间，所以切开后两边都是闭合的。
- 仅有第一条 user 或旧摘要的前缀没有可总结的进展，不做摘要、不写 compaction；手动命令提示「没有可压缩的旧历史」。
- **切点可以落在当前轮中间**：一轮里调了上百次工具时，只能在批与批之间切。尾部以 assistant 开头也合法（前面会是
  摘要这条 user 消息）。

**② 摘要请求**

```
prefix = 压缩前的 entries[0 ..< c]，保留尚未裁剪的工具输出原文
request.messages = [system: compact.md] + prefix + [user: 「请按要求总结以上对话。」]
request.tools = []                        # 不带工具
request.max_tokens = gateway 的 max_tokens
```

- 第一级在本次操作里尚未提交的裁剪仍可供摘要读取原文；更早已经提交的裁剪保持占位，不能恢复原文。
- 整个摘要请求超过 `limit` 时，先从最旧的尚未裁剪的 tool 输出开始换成占位，够用就停止。
  仍然过大才从最旧的 assistant 开始整批丢弃（连同其全部 tool 消息）；用户请求和旧摘要最后才丢。
  丢弃消息时在最后那条 user 中注明「更早的对话已丢弃」。这些缩减只影响摘要请求，不直接修改历史。
- 缩减后仍超预算或已没有 assistant 消息时，不调用模型写只有用户请求的摘要，按摘要失败退化处理。
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
尚有原文的工具输出里，提取用户任务需要的事实并放入「当前进度」，包括 assistant 还没写下的 namespace、接口和报错等。

### 4.5 失败与退化

| 情况 | 处理 |
| --- | --- |
| 摘要请求 `rejected` / `exhausted` / `context_too_long` | 退化：从最前面**整批**丢弃（保留摘要消息），直到估算 ≤ target；`Notice(warn)`「摘要失败，已丢弃最早的 N 条消息」；记录为 `compaction`，summary 为空 |
| 摘要请求被取消 | 历史不变，抛 `cancelled`，本轮以 interrupted 结束 |
| 摘要比 prefix 还长 | 照样替换（极少见）；如果仍超过 trigger，下一步会再压一次 |
| 没有可用切点（如当前批之前只有 user 或旧摘要） | 不做摘要，只能靠第一级；仍然超长就以 `failed` 结束，提示 `/new` |

裁剪、摘要先在历史副本上完成，成功提交时才写 `prune` / `compaction`，所以摘要取消不会留下第一级裁剪。
失败退化也遵守保护区：有旧摘要则保留；没有摘要时只能丢到一条 user 消息之前，以保持 I4。
没有可安全丢弃的消息时发警告；没有任何压缩进展且仍超过可用窗口时以 `failed` 结束。
摘要比原文更长或保护区本身过大时，不为凑预算破坏保护区；服务端仍拒绝则走单次强制压缩上限。
小预算下，单次文件输出加固定提示词可能已经超过预算。摘要会尽量利用保留下来的工具原文，但不能从已提交裁剪的占位
重新推导文件内容；system prompt 要求模型先写下后续任务需要的要点，信息已省略且没有明确记录时重新调用，不凭记忆描述。

---

## 5. 触发点

| 触发 | 做什么 |
| --- | --- |
| 每一步请求前，估算 > trigger | 第一级，不够再第二级 |
| 服务端报 `context_too_long`（估算偏低） | `force`：第一级裁掉保护区以外的全部，再第二级；然后重发这一步。同一步第二次超长 → `failed` |
| `/compact` | 第二级 |

自动压缩在发出 `StepStarted` 之前完成；强制压缩在同一步内、重发之前。压缩提交时记一条 info 日志（模式、前后
tokens、裁剪条数、是否摘要），摘要失败退化和服务端报超长记 warn。

`Agent::compact` 返回 `TurnStatus`，不创建一轮、不写 user / turn_end；完成后同步会话记录。
Shell 在 agent 线程执行命令，Esc / Ctrl+C 可取消，完成后继续处理输入队列。

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
