# 10 McpHub

> 里程碑：C6 · 头文件 `agent/mcp_hub.hpp` · 实现 `mcp_hub.cpp` · 依赖 mcp、tools（`add_mcp`、`remove_prefix`）

管理一个会话里所有 MCP server 的生命周期：后台连接、把工具合并进 Registry、响应 `list_changed`、断线重连、退出时
清理。mcp 模块只管「一个 Client 连接一个 server」；多个 server 怎样和 agent 的节奏配合，在这里。

C6 之前 `Setup::mcp_servers` 被忽略（入口可以在有配置时记一条 info「MCP 将在后续版本接入」）。

---

## 1. 职责

**做**：并行连接、不阻塞启动；只在 agent 线程的安全点改 Registry；刷新与重连；状态给界面显示。

**不做**：MCP 协议（mcp 模块）；工具结果转文本（tools 层）；是否允许调用（Policy：MCP 工具一律询问）。

---

## 2. 接口

```cpp
namespace dagent::agent {

struct ServerState {
    std::string name;
    enum class Status { connecting, ready, failed, disconnected, reconnecting } status;
    std::size_t tools = 0;       ///< ready 时的工具数
    std::string error;           ///< failed / disconnected 的原因
};

class McpHub {
public:
    McpHub(std::vector<mcp::ServerConfig>, mcp::Options);   ///< 立即为每个 server 起连接线程
    ~McpHub();                                               ///< request_stop 所有连接线程并 join

    /// agent 线程、两次模型请求之间调用：合并已完成的连接、处理 list_changed 和断线。
    void apply_pending(tools::Registry&, const Sink&, std::stop_token);

    /// 调度器看到 McpView::disconnected 时调用。
    void mark_disconnected(std::string_view server, std::string reason);

    std::vector<ServerState> states() const;                 ///< 线程安全
};

} // namespace dagent::agent
```

---

## 3. 状态机

```
            connect 成功
connecting ────────────► ready ◄──────────────┐
    │                     │                    │ 重连成功
    │ connect 失败         │ 调用返回 disconnected │
    ▼                     ▼                    │
  failed           disconnected ──apply_pending──► reconnecting
                                                   │ 重连失败
                                                   ▼
                                                 failed
```

- 每个 server **只自动重连一次**：`failed` 之后不再尝试，直到下次启动。一个一直起不来的 server 反复重连只会拖慢
  每一步。
- `failed` 和 `disconnected` 都发一次 `Notice(warn)`，带 server 名和原因（`McpError::what()`，mcp 模块保证不含密钥）。

---

## 4. 线程与数据

```cpp
struct Server {
    mcp::ServerConfig config;
    std::unique_ptr<mcp::Client> client;     // 只在 agent 线程上装入 / 销毁
    std::jthread connector;                  // 连接或重连线程
    std::unique_ptr<mcp::Client> incoming;   // 连接线程放这里，受 mutex 保护
    std::atomic<bool> tools_changed{false};  // on_tools_changed 回调只设这个
    ServerState state;                       // 受 mutex 保护
};
std::mutex mutex_;
std::vector<Server> servers_;
```

| 操作 | 线程 | 做什么 |
| --- | --- | --- |
| 连接 | 连接线程 | `mcp::Client::connect(config, opt, stop)`；成功 → 注册 `on_tools_changed`，放进 `incoming`；失败 → `state = failed` |
| `on_tools_changed` | stdio 读取线程 / HTTP 调用线程 | 只设 `tools_changed = true`（mcp 文档：回调里不要阻塞，不要调 Client 方法） |
| `apply_pending` | agent 线程 | 见 §5 |
| `mark_disconnected` | agent 线程 | `state = disconnected` |
| `states` | 任意（界面） | 加锁拷贝 |

---

## 5. apply_pending

```
for s in servers:
    lock
    if s.incoming:                                   # 新连上（或重连上）的
        registry.remove_prefix("mcp__" + name + "__") # 重连时先去掉旧工具
        s.client = move(s.incoming)                  # 旧 Client 在这里析构——必须在 remove_prefix 之后
        tools::add_mcp(registry, *s.client)
        s.state = ready（tools = client.tools().size()）
    elif s.state == disconnected:
        registry.remove_prefix("mcp__" + name + "__")
        s.client.reset()
        s.state = reconnecting；启动连接线程
    unlock
    if s.state == ready and s.tools_changed.exchange(false):
        s.client.refresh_tools(opt.connect_timeout, stop)     # 可能抛 McpError
        registry.remove_prefix(…); tools::add_mcp(registry, *s.client)
```

- **顺序是硬性的**：工具引用着 Client，Client 析构之前它的工具必须已经从 Registry 移除。
- `refresh_tools` 在 agent 线程上阻塞（有超时、可取消）；失败 → `mark_disconnected`，下一次 `apply_pending` 走重连。
- `apply_pending` 在每一步开头调用（[04-turn §4](04-turn.md)），所以工具列表只在两次请求之间变化，不会在一批工具
  执行中途变化。

---

## 6. 启动不等 MCP

`npx` / `uvx` 冷启动可能要几十秒，用户应该能立刻开始对话：

- `McpHub` 构造时只起线程，不等待。
- 第一步请求时还没连上的 server，它的工具这一步就没有；连上后下一步自动出现。模型在一轮中途看到新工具是正常的。
- 交互界面的状态栏显示「MCP 2/3」这样的连接进度（`states()`），`failed` 的用警告色。
- run 模式：连接失败只在 stderr 打警告，不影响退出码。

工具列表变化会让 prompt 缓存失效一次，可以接受。

---

## 7. 关闭

`~McpHub`：对所有连接线程 `request_stop()` 并 join（`mcp::Client::connect` 支持 stop，即时返回），然后按声明顺序销毁
Client（stdio server 子进程由 exec 清理进程组）。

`Agent` 里 `hub_` 声明在 `registry_` 之前（[04-turn §3](04-turn.md)），`registry_` 先析构，工具不会悬空。

---

## 8. 边界情况

| 情况 | 处理 |
| --- | --- |
| 配置里的命令不存在 | `McpError::spawn` → failed + 警告；其他 server 照常 |
| server 连上了但一个工具都没有 | ready，`tools = 0`，状态栏照常显示 |
| 调用中 server 进程被杀 | 这次调用拿到 `disconnected` 的结果（tools 层已转成 is_error）；调度器 `mark_disconnected`；下一步前自动重连 |
| 重连后工具列表变了 | `remove_prefix` + `add_mcp`，以新列表为准 |
| 一轮进行中用户按 Esc | 不影响连接线程（它们有自己的 stop_source，只在 Hub 析构时停） |
| 两个 server 清理后名字冲突 | app 在加载配置时已经报错（app 文档） |

---

## 9. 验收

- plan 场景 19：`temp/tools_check/tools_server.py` 作 stdio server，给它加一个启动延迟（`sleep 5`）——启动后立即能输入；
  5 秒后下一步请求里出现它的工具，模型能调用；`kill` 掉 server 进程后调用得到断连提示，下一步前自动重连，再调用成功。
- plan 场景 20：配置一个命令不存在的 server——只有一条警告，其余工具正常；状态栏显示它 failed。
