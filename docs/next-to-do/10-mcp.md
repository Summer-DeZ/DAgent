# 10 McpHub

> 里程碑：C6 · 头文件 `agent/mcp_hub.hpp` · 实现 `mcp_hub.cpp` · 依赖 mcp、tools（`add_mcp`、`remove_prefix`）

管理一个会话里所有 MCP server 的生命周期：后台连接、把工具合并进 Registry、响应 `list_changed`、断线重连、退出时
清理。mcp 模块只管「一个 Client 连接一个 server」；多个 server 怎样和 agent 的节奏配合，在这里。

C6 已接入 `Setup::mcp_servers`：Agent 创建及恢复时启动连接，每一步请求前合并工具，界面显示实时连接状态。

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

    /// agent 线程、两次模型请求之间调用：为断开的 server 启动重连，等连接中 / 重连中的 server 出结果
    /// （最多 connect_timeout，可取消），再合并已完成的连接、处理 list_changed。
    void apply_pending(tools::Registry&, const Sink&, std::stop_token);

    /// 调度器看到 McpView::disconnected 时调用；返回追加到这个调用结果末尾、给模型看的说明（T12 / T13），
    /// 这个 server 不是刚断开时返回空。
    std::string mark_disconnected(std::string_view server, std::string reason);

    /// 单次交付尚未报告的警告，不刷新、不等待连接；Agent 在一轮结束时也调用。
    void report_pending(const Sink&);

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
- 初始连接失败、首次断开和重连失败都各报告一次 `Notice(warn)`，带 server 名和原因；同批调用重复报告断开会被合并。
  重连成功后再断开直接进入 failed，警告注明不再自动重连。
- 连接线程只把警告放入队列；agent 线程在 `apply_pending` 和一轮结束时交付，不把临时 Sink 留给后台线程。
- 模型也要知道：断开的那次调用结果末尾追加 T12「下一步开始前会自动重连一次」或 T13「本次会话不再重连，工具已移除」
  （[03-conversation §5](03-conversation.md)）。否则工具从列表里消失后，模型会反复寻找它。

---

## 4. 线程与数据

```cpp
struct Server {
    mcp::ServerConfig config;
    std::atomic<bool> tools_changed{false};  // 回调只设标志，声明在 Client 前，确保比 Client 活得久
    std::unique_ptr<mcp::Client> client;     // 只在 agent 线程上装入 / 销毁
    std::unique_ptr<mcp::Client> incoming;   // 连接线程放这里，受 mutex 保护
    ServerState state;                       // 受 mutex 保护
    bool retried = false;                    // agent 线程维护，整个会话只重连一次
    std::jthread connector;                  // 连接或重连线程
};
std::mutex mutex_;
std::vector<std::unique_ptr<Server>> servers_; // Server 地址稳定，供线程和回调引用
```

| 操作 | 线程 | 做什么 |
| --- | --- | --- |
| 连接 | 连接线程 | `mcp::Client::connect(config, opt, stop)`；成功 → 注册回调，交接 incoming，并发布 ready / 工具数；失败 → failed / 警告队列 |
| `on_tools_changed` | stdio 读取线程 / HTTP 调用线程 | 只设 `tools_changed = true`（mcp 文档：回调里不要阻塞，不要调 Client 方法） |
| `apply_pending` | agent 线程 | 见 §5 |
| `mark_disconnected` | agent 线程 | `state = disconnected` |
| `states` | 任意（界面） | 加锁拷贝 |

---

## 5. apply_pending

```
report_pending(sink)
do:
    # ① 断开 / 失败的：先摘工具再销毁 Client；断开的启动唯一一次重连
    for s in servers where status ∈ {disconnected, failed}:
        registry.remove_prefix("mcp__" + name + "__")
        s.client.reset()
        if status == disconnected: s.retried = true；锁内设 reconnecting；锁外启动连接线程
    # ② 等连接中 / 重连中的 server 出结果
    if 有 connecting / reconnecting: sink(Notice(info)「等待 MCP 服务连接：a、b」)
        条件变量等到都不再是 connecting / reconnecting，最多 connect_timeout；stop → 抛 cancelled
    # ③ 合并
    for s in servers:
        incoming = 锁内 move(s.incoming)
        if incoming:                                     # 新连上（或重连上）的
            registry.remove_prefix(…)                    # 重连时先去掉旧工具
            s.client = move(incoming)                    # 旧 Client 在这里析构——必须在 remove_prefix 之后
            tools::add_mcp(registry, *s.client)
        if s.client and s.tools_changed.exchange(false):
            s.client.refresh_tools(opt.connect_timeout, stop)     # 失败 → mark_disconnected
            registry.remove_prefix(…); tools::add_mcp(registry, *s.client)；锁内更新工具数
while ③ 里有 server 因刷新失败被标成断开                 # 每个 server 至多断开两次，循环有界
report_pending(sink)
```

- **顺序是硬性的**：工具引用着 Client，Client 析构之前它的工具必须已经从 Registry 移除。
- `refresh_tools` 在 agent 线程上阻塞（有超时、可取消）；失败 → `mark_disconnected`，同一次 `apply_pending` 里回到 ①
  移除工具并重连或终止。
  取消时保留 tools_changed 标志，抛 cancelled 使本轮 interrupted，下轮可再刷新；取消不消耗重连机会。
- 锁内只交接 incoming、状态和警告；连接、刷新、Client 析构、join 和 Sink 调用都在锁外。
- `apply_pending` 在每一步开头调用（[04-turn §4](04-turn.md)），所以工具列表只在两次请求之间变化，不会在一批工具
  执行中途变化。

---

## 6. 启动不等 MCP，第一次请求等

`npx` / `uvx` 冷启动可能要几十秒，用户应该能立刻开始输入；但模型请求不能在工具还没到位时发出：

- `McpHub` 构造时只起线程，不等待；界面照常进入、可以输入。
- 每一步请求前，`apply_pending` 等连接中 / 重连中的 server 出结果（最多 `mcp.connect_timeout_ms`，Esc / 信号可取消），
  并发一条 `Notice(info)`「等待 MCP 服务连接：…」。否则第一步请求里没有 MCP 工具，模型会直接回答「没有这个工具」，
  run 模式还会以 0 退出；断开后的下一步也会缺少正在重连的工具。
- 代价：一个握手卡住的 server 会让它之后的第一次请求多等 `connect_timeout`；之后它是 failed，不再等待。
  需要更快失败时调小 `mcp.connect_timeout_ms`。
- 交互界面的状态栏显示「MCP 2/3」这样的连接进度（`states()`），`failed` 的用警告色。
- `ready` 表示连接和工具发现已完成；Registry 仍等下一个 agent 安全点才更新。`Agent::mcp_states()` 把线程安全快照提供给 Shell。
  界面在连接中或任务忙碌时每 200 ms 刷新，稳定空闲后停止轮询；failed / disconnected 显示名字并用警告色，连接中 / 重连中显示「连接中」「重连中」，等待期间能看到在等谁。
- run 模式：连接失败在 stderr 打警告，不影响退出码；jsonl 另保留对应的结构化 Notice。
  空闲界面可先看到失败状态，警告事件在下一次 agent 安全点交付。

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
| 调用中 server 进程被杀 | 这次调用拿到 `disconnected` 的结果（tools 层已转成 is_error），末尾追加 T12；调度器 `mark_disconnected`；下一步请求前重连并等到结果 |
| 重连成功后再次断开 | 结果末尾追加 T13；failed，工具移除，不再重连 |
| server 握手卡住 | 第一次请求等满 `connect_timeout` 后不带它继续；连接线程超时后 failed + 警告 |
| 重连后工具列表变了 | `remove_prefix` + `add_mcp`，以新列表为准 |
| 一轮进行中用户按 Esc | 不影响连接线程（它们有自己的 stop_source，只在 Hub 析构时停） |
| 两个 server 清理后名字冲突 | app 在加载配置时已经报错（app 文档） |

---

## 9. 验收

- plan 场景 19：一个最小的 stdio MCP server（官方 Python SDK 写，临时放在 `temp/` 下），给它加一个启动延迟（`sleep 5`）——启动后立即能输入；
  在它连上之前发出的第一条消息等它连上，第一步请求里就有它的工具，模型能调用；`kill` 掉 server 进程后调用得到断连提示
  和 T12，下一步请求前重连完成，再调用成功；第二次断开得到 T13，模型不再寻找这些工具。
- plan 场景 20：配置一个命令不存在的 server——只有一条警告，其余工具正常；状态栏显示它 failed。
