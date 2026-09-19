# 03 Conversation：消息历史

> 里程碑：C1（C5 补压缩用的接口）· 头文件 `agent/conversation.hpp` · 实现 `conversation.cpp`

发给模型的消息历史。它是整个核心里最需要「永远正确」的数据结构：历史一旦违反协议（比如 tool_call 没有对应的
tool 消息），下一次请求直接 400，而且之后每一次都会 400，会话就废了。

---

## 1. 职责

**做**：保存消息和每条消息的元数据（序号、是否被裁剪、token 估算）；检查并维持不变式；组装 `Request`；给压缩提供
切点和替换操作。

**不做**：决定什么时候压缩（[07-context](07-context.md)）；落盘（[09-record](09-record.md)）。Conversation 是纯内存
数据结构，不做 I/O，不发事件。

---

## 2. 不变式

每次 `build` 之前必须全部成立：

| 编号 | 内容 | 违反的后果 |
| --- | --- | --- |
| **I1 闭合** | 每条带 tool_calls 的 assistant 消息后面，紧跟着**恰好**每个调用一条 tool 消息，顺序与 tool_calls 一致 | OpenAI 协议 400 |
| **I2 非空** | 不存在 content、tool_calls 都为空的 assistant 消息（只有 reasoning 也算空） | 部分网关 400 |
| **I3 归属** | tool 消息只出现在 I1 描述的位置，`tool_call_id` 一定能在前面那条 assistant 里找到 | 400 |
| **I4 开头** | 第一条是 user 消息（用户输入或压缩摘要） | Qwen 等模板要求 system 之后先是 user |
| **I5 system 不在历史里** | system prompt 由 `build` 放在最前面，不存在 entries 里 | 压缩会误伤它 |
| **I6 UTF-8** | 所有文本都是合法 UTF-8 | nlohmann `dump()` 抛异常，请求发不出去 |

**「打开」状态**：`add_assistant` 加入一条带 tool_calls 的消息之后，到最后一个 `add_tool_result` 之前，历史处于
打开状态，I1 暂时不成立。只有调度器（[05-dispatch](05-dispatch.md)）会让历史处于打开状态，而且它保证返回前闭合；
任何提前退出的路径都用 `open_calls()` 找出没答复的调用，逐个补上标准文本（§5）。

---

## 3. 接口

```cpp
namespace dagent::agent {

struct Entry {
    Message message;
    std::int64_t ordinal = -1;  ///< 会话记录里的序号（09-record）；压缩摘要是 -1
    bool pruned = false;        ///< tool 消息的内容已被裁剪成占位（07-context）
    std::string summary;        ///< tool 消息：调度时的 Intent::summary，生成裁剪占位用
    std::size_t tokens = 0;     ///< estimate_tokens 的缓存，加入或修改时算一次
};

class Conversation {
public:
    // ---- 追加（每个都返回新条目的 ordinal）----
    std::int64_t add_user(std::string text);
    std::int64_t add_assistant(Message message);          ///< 可带 tool_calls；进入打开状态
    std::int64_t add_tool_result(std::string_view call_id, std::string text, std::string summary);

    /// 当前打开的 assistant 里还没有 tool 消息的调用，按 tool_calls 顺序。闭合时为空。
    std::vector<ToolCall> open_calls() const;

    // ---- 读取 ----
    const std::deque<Entry>& entries() const;
    std::size_t tokens() const;                           ///< 所有 entry.tokens 之和
    std::int64_t next_ordinal() const;

    /// system 放最前面，其后是全部 entries 的 message。要求闭合。
    Request build(const std::string& system, const std::vector<ToolDef>& tools, const ModelParams&) const;

    /// 检查 I1–I4；返回第一条违反的描述。debug 构建里 build 开头 assert 它为空。
    std::optional<std::string> validate() const;

    // ---- 压缩（07-context）----
    std::vector<std::size_t> safe_cuts() const;            ///< 可以切开的下标，升序
    void prune(std::size_t tool_entry, std::string placeholder);
    void replace_prefix(std::size_t cut, std::string summary_message);

    // ---- 恢复（09-record）----
    void restore(Entry entry);                             ///< 按记录原样放回，不重新分配 ordinal
    void set_next_ordinal(std::int64_t);

private:
    std::deque<Entry> entries_;
    std::int64_t next_ordinal_ = 0;
};

} // namespace dagent::agent
```

- `ordinal` 由 Conversation 分配（每次 `add_*` 加一），Recorder 把它写进记录（[09-record §3](09-record.md)）。恢复时
  用 `restore` 按记录原样放回，保证恢复前后的序号一致——压缩记录里的 `keep_from` 靠它对齐。
- `add_tool_result` 的 `call_id` 必须等于 `open_calls().front().id`，否则是调度器的编程错误（assert）。
- 用 `std::deque`：压缩要从头部删除。

---

## 4. build

```
Request r
r.model / max_tokens / temperature ← ModelParams
r.messages = [ Message{system, system_prompt} ] + [ e.message for e in entries ]
r.tools = tools            # Registry::specs() 转换而来，顺序稳定
r.stream = true
```

- 每次 `build` 都完整拷贝一遍历史。几百条消息、几十万字节的拷贝相对一次网络请求可以忽略，不值得为它引入共享结构。
- `reasoning_content` 总是保存在 Message 里；发不发由 codec 的 `send_reasoning_content` 决定，Conversation 不管。

### 缓存友好

DeepSeek 等网关按前缀缓存 prompt，前缀一变缓存就失效，费用和延迟都会上去：

- 历史**只追加**，除压缩外不回改旧消息。
- system prompt 在会话开始时渲染一次，整个会话不变（[08-prompt](08-prompt.md)）。
- 工具列表顺序由 `Registry::specs()` 保证稳定；MCP 工具增删会让缓存失效一次，可以接受。
- 占位、拒绝等标准文本是固定字符串，不带时间戳之类每次都变的内容。

---

## 5. 标准文本

核心自己生成、会进入历史给模型看的文字，全部集中在这里（实现里放在 `conversation.cpp` 顶部的常量里，不散落）：

| 编号 | 场景 | 文本 | 放在 |
| --- | --- | --- | --- |
| T1 | 回复被中断 | `\n\n[回复被用户中断]` | 追加在 assistant content 末尾 |
| T2 | 用户中断，调用未执行 | `用户中断了本轮，这个调用没有执行。` | tool |
| T3 | 用户拒绝 | `用户拒绝了这次调用。不要换一种方式绕过它，等待用户的进一步指示。` | tool |
| T4 | 用户拒绝并说明 | `用户拒绝了这次调用，并说明：{feedback}` | tool |
| T5 | 前面的调用被拒 | `同一批里前面的调用被用户拒绝，这个调用没有执行。` | tool |
| T6 | 策略拒绝（run 模式） | `权限策略拒绝了这次调用：{reason}。当前是非交互模式，无法向用户确认；请换一种不需要这个权限的做法，或在最终回复里说明需要用户做什么。` | tool |
| T7 | 未知工具 | `未知工具 {name}。可用的工具：{names}` | tool |
| T8 | 工具调用超额 | `本轮工具调用已达上限（{n} 次），这个调用没有执行。请总结目前的进展，并告诉用户还有什么没做完。` | tool |
| T9 | 崩溃后闭合 | `会话在执行这个调用时意外中断，结果未知。如果它会修改文件或状态，请先检查当前状态再继续。` | tool |
| T10 | 压缩摘要 | 见 [07-context §4.3](07-context.md) | user |
| T11 | 裁剪占位 | 见 [07-context §4.2](07-context.md) | tool（替换原内容） |
| T12 | MCP 断开、将重连 | `\n\nMCP 服务 {server} 已断开。下一步开始前会自动重连一次，成功后它的工具会重新出现。` | tool（追加在断开的调用结果末尾） |
| T13 | MCP 不再可用 | `\n\nMCP 服务 {server} 已不可用，本次会话不再重连，它的工具已移除。不要再尝试调用这些工具，改用其他办法或向用户说明。` | tool（同上） |

原则：告诉模型**发生了什么**和**接下来该怎么做**；不道歉、不重复用户看得到的信息。

---

## 6. 边界情况

| 情况 | 处理 |
| --- | --- |
| 中断发生在第一个 token 之前 | 不调 `add_assistant`（I2） |
| 中断发生在流式输出中，已有文本 | `add_assistant` 只带文本 + T1，丢弃不完整的 tool_calls 和 reasoning |
| 模型只返回了 reasoning | 按空回复处理，不加入历史 |
| 同一条回复里两个调用 id 相同 | Model 已经改名（[02-model §3.1](02-model.md)），这里不会遇到 |
| 用户输入是空串 | 入口层就不提交（界面忽略空输入，run 模式参数错误退出 2） |
| 连续两条 user 消息（中断后再输入、压缩摘要后接用户输入） | 合法，OpenAI 兼容网关都接受 |

---

## 7. 验收

Conversation 没有单独的验收场景，它的正确性由所有「退出路径」场景共同检验：正常完成（plan 场景 1）、中断
（场景 8）、拒绝（场景 6）、崩溃恢复（场景 11）、压缩（场景 16、17）——每个场景之后的下一次请求都不能 400。
审核时我会在每个场景后跑一次 `validate()`。
