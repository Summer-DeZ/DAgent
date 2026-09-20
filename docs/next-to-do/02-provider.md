# 02 Provider 抽象

> 里程碑 P2 · 新增 `src/public/agent/provider.hpp` 与 `src/private/agent/provider.cpp`

把「模型怎么连、报文长什么样」收进一个对象，核心只拿着它的配置和一个工厂函数。
**这一步是纯重构，行为必须和现在完全一致。**

---

## 1. 接口

```cpp
// agent/provider.hpp
namespace dagent::agent {

/// 一套模型配置：app 从配置文件映射过来，Agent 只认它。
struct ProviderConfig {
    std::string kind = "openai-chat"; ///< 见 provider_kinds()
    std::string name;                 ///< 配置里的名字，显示用（"local" / "sonnet"）
    std::string base_url;             ///< 空则用该 kind 的默认端点
    std::string model;
    std::string api_key;              ///< app 从 Secrets 取出后填；不进日志
    std::size_t max_tokens = 0;
    double temperature = -1.0;
    std::size_t context_window = 0;   ///< 0 = 回落到 context.window_tokens
    bool send_reasoning_content = false;
    bool include_usage = true;
    nlohmann::json extra_body = nlohmann::json::object(); ///< 原样透传
};

/// 流的分帧方式：SSE 或按行的 NDJSON。
enum class Framing : std::uint8_t { sse, ndjson };

/// 每个 kind 的固定信息，供校验、装配与界面显示。
struct ProviderInfo {
    std::string_view kind;
    std::string_view default_base_url; ///< 可以为空（必须由用户给）
    Framing framing = Framing::sse;
    bool needs_api_key = false;
    bool needs_max_tokens = false;     ///< anthropic 的 max_tokens 必填
};

std::span<const ProviderInfo> providers() noexcept;
const ProviderInfo* find_provider(std::string_view kind) noexcept;

/// 造一个编解码器；kind 不认识时抛 std::invalid_argument。
std::unique_ptr<Codec> make_codec(const ProviderConfig&);

} // namespace dagent::agent
```

## 2. Setup 的改动

```diff
 struct Setup {
     Options options;
-    ModelParams model;
-    OpenAiChatOptions codec;
+    ProviderConfig provider;   ///< model / max_tokens / temperature 都在里面
     net::HttpOptions http;
```

- `ModelParams` 里的三个字段并进 `ProviderConfig`，`Request` 的装配从它取值。
- `agent.cpp:50` 的 `make_openai_chat_codec(setup_.codec)` 换成 `make_codec(setup_.provider)`；
  仍然是**每次请求一个新实例**（Codec 有状态）。
- 会话记录里的模型名沿用 `provider.model`；`Agent::resume` 比较模型名的那段不用改。
- `OpenAiChatOptions` 与 `make_openai_chat_codec` 降级为 provider 内部实现，
  不再出现在 `Setup` 或 app 的接口上。

## 3. 谁负责什么

| 关注点 | 归属 |
| --- | --- |
| URL、鉴权头、请求体形状 | Provider 的 `encode` |
| 流事件 → `StreamEvent` | Provider 的 `decode` |
| 非 2xx → `Error`（可重试、上下文超长、retry_after） | Provider 的 `classify` |
| 重试节奏、超时、取消 | `Model`（不变） |
| 历史不变式、压缩、工具循环 | `Agent` / `Conversation`（不变） |
| 密钥来源与配置校验 | `app`（[03](03-config.md)） |

**Provider 不做**：重试、压缩、日志脱敏之外的任何策略。它只翻译。

## 4. 流分帧

`Model::call` 现在固定用 `net::SseParser` 喂 `codec->decode(SseEvent)`。
Ollama 原生是 NDJSON（每行一个完整 JSON，没有 `data:` 前缀），所以分帧方式要由 provider 声明：

- `Framing::sse`：保持现状。
- `Framing::ndjson`：`Model` 按 `\n` 切行，把每行包成 `net::SseEvent{.data = 行内容}` 再交给 `decode`。
  这样 `Codec` 接口不用改，只有 `Model` 里多一个分支。

P2 只需要把分支留出来（`framing == sse` 之外的路径暂时 `throw`），真正用到是在做原生 Ollama 时。

## 5. 风险

| 风险 | 预案 |
| --- | --- |
| 重构顺手改了行为 | P2 不允许改任何请求体字段；用同一个本地模型跑同一句话，对比改前改后的日志 |
| `extra_body` 语义各家不同 | 只保证「原样合并进请求体顶层，不覆盖已生成的字段」，写进文档 |
| Codec 有状态被复用 | `make_codec` 每次请求调用一次；不要缓存实例 |

## 6. 完成标准

1. `grep -r "openai" src/public/agent` 只剩 provider 内部实现的注释，`Setup` 里没有厂商类型。
2. 本地模型跑一轮带工具调用的对话，与改造前表现一致（含 usage、finish、错误分类）。
3. `dagent run` 与交互模式都通过一次真实运行。
