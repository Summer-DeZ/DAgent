# 11 入口：装配、main 与 run 模式

> 里程碑：C1（Options 迁移、装配、run 模式 text 输出）· C2（json / jsonl、退出码、信号）· C3（sessions、trust、恢复参数）· 头文件 `agent/options.hpp`、`agent/headless.hpp` · 实现 `headless.cpp`、`main.cpp`（可执行 `dagent`，不进库）

把 app 已经解析好的 `Args` 和 `Config` 变成一个 `Agent`，按模式交给 run 模式或交互界面。app 模块负责「读懂配置和
命令行」，这里负责「用它们把东西组装起来」。

---

## 1. Options：从 app 迁入

app 原先自己定义了 `ContextOptions`、`RunOptions`、`ProgressOptions` 和 `permissions` 字符串；C1 已把它们搬到
`agent/options.hpp`：

```cpp
namespace dagent::agent {

struct ContextOptions { … };        // 07-context §2，字段和默认值与 app 现有的一致

struct Limits {                     // config "run" 段
    int max_model_calls = 24;
    int max_tool_calls = 35;
    int max_model_retries = 2;
};

struct ProgressOptions {            // config "progress" 段
    std::chrono::milliseconds interval{1000};   ///< run 模式心跳、界面计时刷新
};

struct Options {
    ContextOptions context;
    Limits run;
    ProgressOptions progress;
    PermissionMode permissions = PermissionMode::automatic;   ///< 只作用于 run 模式（06-permission §7）
};

} // namespace dagent::agent
```

- `app::Config` 里的 `context`、`run`、`progress`、`permissions` 换成一个 `agent::Options agent`；app 的映射代码改为填它，
  键名不变。`permissions` 只接受 `"auto"`、`"deny"`，其他值抛 `ConfigError{type}`。
- `dagent_app` 改为链接 `dagent_agent`。agent 不反向依赖 app。
- `run` 的上限默认值按 C1.10 的实测（场景 1 用 7–8 步、11 次工具调用）定为 24 / 35（[plan §2](plan.md)）。

---

## 2. Setup：一个 Agent 需要的全部输入

```cpp
namespace dagent::agent {

struct Setup {
    Options options;

    // 模型
    ModelParams model;
    OpenAiChatOptions codec;               ///< 含 api_key
    net::HttpOptions http;                 ///< 已按 02-model §6 调整：timeout = 0

    // 工作区
    std::filesystem::path cwd;             ///< 工作区根（Args::cwd）
    std::filesystem::path project_root;    ///< Config::project_root
    std::optional<std::filesystem::path> git_root;

    // 外围
    tools::Options tools;
    workspace::FileOptions files;
    workspace::SearchOptions search;
    exec::Options process;
    session::Options session;
    mcp::Options mcp;
    std::vector<mcp::ServerConfig> mcp_servers;

    // 运行环境
    exec::Support sandbox;                 ///< exec::probe()，启动时探测一次
    PermissionMode permission_mode;        ///< 交互：ask；run：--permissions 或配置
    std::optional<std::string> system_prompt_override;  ///< gateway.system_prompt_file 的内容，已读好
};

} // namespace dagent::agent
```

### 2.1 从 Config 到 Setup

在 `main.cpp` 里的 `make_setup(const app::Config&, const app::Args&, PermissionMode)`：

| Setup | 来源 |
| --- | --- |
| `options` | `config.agent` |
| `model` | `gateway.model`、`gateway.max_tokens`、`gateway.temperature.value_or(-1)` |
| `codec` | `gateway.base_url`、`gateway.api_key`、`gateway.send_reasoning_content`、`gateway.include_usage`、`gateway.extra_body`（[02-model §7](02-model.md)） |
| `http` | `config.http`，`timeout` 改成 0；`idle_timeout` 为 0 时补 120 秒 |
| `cwd` | `args.cwd` |
| `project_root`、`git_root` | `config.project_root`；它是 git 根时 `git_root` 同值 |
| 外围 Options | `config.tools / files / search / process / session / mcp`、`config.mcp_servers` |
| `sandbox` | `exec::probe()` |
| `system_prompt_override` | `gateway.system_prompt_file` 非空时读文件；读不到 → 启动失败 |

**进程不 chdir**（app 文档的约定），所有路径都从 `Setup::cwd` 显式传下去：`tools::Context` 的 root、沙箱 writable、
`collect_environment`、`session::Meta::cwd` / `git_root`。

---

## 3. main

```
main(argc, argv):
    屏蔽 SIGINT / SIGTERM（§4.4；必须在任何线程创建之前，spdlog 也会建线程）
    args = app::parse_args(argc, argv)                   # int → 直接返回（--help、--version、参数错误）

    trust    → app::trust_project(app::project_root(args.cwd))；打印「已信任 <根>」→ 0
    其余：
        secrets = app::load_secrets(app::project_root(args.cwd))
        config  = app::load_config({cwd, config_file, overrides}, secrets)   # ConfigError → stderr，退出 2
        base::init_log(config.log，level ← args.log_level，交互模式 also_stderr = false)
        sessions → 打印列表（§5）→ 0
        run      → return run_headless(make_setup(…, run 的权限), headless 选项)
        interactive:
            if config.untrusted_files 非空: 询问是否信任（§3.1）；是 → trust_project + 重新 load_config
            return ui::run_interactive(make_setup(…, ask), 交互选项, interrupts)
```

### 3.1 信任询问

在**进入全屏界面之前**，用普通的终端输入输出问（此时 stdin 一定是终端，app 已检查）：

```
这个项目有以下配置文件，但项目还没有被信任，所以它们没有生效：
  /path/to/repo/.dagent/config.json
  /path/to/repo/.mcp.json
这些文件可以更改模型网关地址、启动任意命令。只有你确认来源可靠时才信任它。
信任 /path/to/repo 吗？[y/N]
```

放在界面之外做，比在 TUI 里做一个浮层简单，而且信任之后要重新 `load_config`，本来就应该在装配之前完成。

### 3.2 退出码

| 情况 | 退出码 |
| --- | --- |
| 正常（done） | 0 |
| 本轮 failed / limit / denied | 1 |
| 启动失败（提示词模板错、会话找不到、恢复失败） | 1 |
| 参数错误、配置错误 | 2 |
| 被 Ctrl+C 中断 | 130 |

---

## 4. run 模式（`agent/headless.hpp`）

```cpp
namespace dagent::agent {

struct HeadlessOptions {
    enum class Output { text, json, jsonl } output = Output::text;
    std::string prompt;                   ///< app 已经合并了 stdin
    std::optional<std::string> resume_id;
    bool continue_last = false;
};

int run_headless(Setup, const HeadlessOptions&, Interrupts&);   ///< 返回退出码

} // namespace dagent::agent
```

`HeadlessOptions` 由 main 从 `app::Args` 填，agent 库因此不需要认识 app 的类型。

### 4.1 流程

```
run_headless(setup, opt):
    启动信号线程（§4.4）
    agent = 新建 / --resume / --continue（恢复时的 replay Sink 什么都不输出）
    status = agent.run_turn(opt.prompt, sink, Approver{}, stop)
    按 output 格式收尾（§4.2）
    return 退出码（§3.2）
```

- 权限模式：`--permissions` → 配置的 `permissions` → `automatic`。run 模式不会询问，所以 Approver 为空
  （[01-events §5](01-events.md)）。
- 项目未受信任：只 warn，继续（app 文档的约定）。

### 4.2 输出

| 格式 | stdout | stderr |
| --- | --- | --- |
| `text` | **结束时**打印最后一条回复的正文 | 进度：每个工具一行 `→ <summary>`，结束时 `✓` / `✗`；`Notice`；`Retrying`；等待模型超过 `progress.interval` 后每个 interval 一行「等待模型… 12s」 |
| `json` | 结束时一个对象（下面） | 同 text |
| `jsonl` | 第一行 `{"type":"session","id":…,"resumed":false}`，之后每个 Event 一行（[01-events §6](01-events.md)） | 启动失败的错误；warn / error 级 `Notice` 另打一行 `[warn] …`（如 MCP 连接失败），info 只在 jsonl 里 |

`json` 的对象：

```json
{"session_id": "…", "status": "done", "error": "", "result": "最后一条回复的正文",
 "steps": 5, "tool_calls": 12, "usage": {"prompt": 51234, "completion": 2345, "cached": 40960},
 "duration_ms": 83412}
```

- **text 模式的 stdout 只放最终结果，不流式输出**：模型重试时已经输出的文本收不回来（`StreamReset`），而
  `dagent run … > out.md` 这种用法需要干净的结果。流式过程在 stderr 上以进度的形式体现。
- 「最后一条回复」= 本轮最后一条 assistant 消息的 content（被中断的话带 T1）。
- stderr 是终端时进度行可以用 `\r` 原地刷新心跳；不是终端时每行独立，方便进日志。

### 4.3 stdout 被关闭

exec 让整个进程忽略 SIGPIPE（exec 文档），`dagent run … | head -1` 时写 stdout 会得到 EPIPE 而不是被信号杀死。
jsonl 模式写失败时 `request_stop()`，结束本轮，退出码 1；text / json 模式只在最后写一次，写失败忽略。

### 4.4 Ctrl+C

`request_stop()` 不是 async-signal-safe，不能在 signal handler 里调。改用 sigwait 线程（`agent/headless.hpp`）：

```cpp
struct Interrupts {
    std::stop_source stop;
    std::atomic<bool> graceful{false};   // run_turn 期间为 true
};
Interrupts& install_interrupts();        // main 第一行调用：屏蔽 SIGINT/SIGTERM，并启动 sigwait 线程
```

- **必须在创建任何线程之前调用**（spdlog、stdin 读取都会建线程）：之后创建的线程都继承这个屏蔽。
- 一轮运行中（`graceful`）：第一次信号 `request_stop()`，第二次 `_Exit(130)`。
- 轮外（加载配置、读 stdin、创建会话、收尾）：收到信号直接 `_Exit(130)`——这时没有需要闭合的历史，而阻塞在 stdin
  读取上的进程只能这样才停得下来。
- **子进程不继承屏蔽**：信号屏蔽会经 fork、exec 原样继承，exec 在子进程里把它清空（exec 设计文档「子进程运行环境」）。
  否则 bash 工具里的 `timeout`、取消时发给进程组的 SIGTERM 全都不生效。

优雅中断的目标：**1 秒内退出**，会话记录闭合（[04-turn §6](04-turn.md)）。取消时 exec 先发 SIGTERM、`kill_grace` 后
再 SIGKILL（exec 设计文档）；普通程序收到 SIGTERM 立刻退出，捕获 SIGTERM 的程序最多多等 `kill_grace`。

交互模式下终端处于 raw 模式，Ctrl+C 是一个按键事件，由界面处理（[12-ui §5](12-ui.md)）；SIGTERM 仍由同一个 sigwait
线程接收。C4 在整个全屏期间设 `graceful`，包含空闲与等待审批；stop 回调向渲染线程投递退出操作，
取消当前轮并调用 `Runtime::quit()`。离开全屏后先还原终端，再 join agent 线程。

---

## 5. sessions 子命令

```
$ dagent sessions
2026-09-19 14:02   修复 exec 前失败时子进程重刷 stdio 缓冲的问题        01a0b514-2b51-7102-94a4-c94eab6cd546
2026-09-19 11:37   给 tools 层写设计文档                                01a0a3f2-8c1e-7a04-b1d2-5e0f7a9c3b11
```

- `session::list(project_root, 20, session_title)`；时间用本地时区，标题按显示宽度截到 50 列（中文算 2 列）。
- 没有会话时打印「这个项目还没有会话」，退出码 0。
- `--resume` 接受 id 的唯一前缀，所以用户复制前 8 位就够了（[09-record §6](09-record.md)）。

---

## 6. app 与配置的改动清单

已完成的行标了「（已完成）」。

| 位置 | 改动 | 里程碑 |
| --- | --- | --- |
| `app/config.hpp` | 删 `ContextOptions`、`RunOptions`、`ProgressOptions`，`Config` 用 `agent::Options agent`；`permissions` 并入 | C1（已完成） |
| `app/config.hpp` | `Gateway` 加 `send_reasoning_content`、`include_usage`、`extra_body`（json 对象）；删 `enable_thinking` | C1（已完成） |
| `app/config.cpp` | 对应的键映射；已知键列表同步更新（未知键会 warn） | C1（已完成） |
| `app/cli.hpp` | `Args::permissions` 改成 `std::optional<std::string>`（06-permission §7） | C2 |
| `config/dagent.json` | 加 `http.idle_timeout_seconds: 120`；`gateway.system_prompt_file` 改成 `../prompts/system.md`；`gateway.enable_thinking` 改成 `extra_body`；开发网关改为 DeepSeek | C1（已完成） |
| `src/CMakeLists.txt` | `dagent_agent` 加源文件与依赖（tools、session、mcp、workspace、exec）；prompts 嵌入（08-prompt §2.1）；新增可执行 `dagent`；`dagent_app` 链接 `dagent_agent` | C1（已完成） |
| `src/CMakeLists.txt` | `dagent_ui` 链接 `dagent_agent`、`tools`；`dagent` 链接 `dagent_ui` | C4 |
| `app/config.*` | 新键 `ui.theme_file`（相对路径相对配置文件解析），main 填进 `ui::InteractiveOptions` | C4 |
| `docs/design/app.md` | 「可执行入口 main 待核心组装」改成指向核心文档；映射表改为 `agent::Options` | C1（已完成） |

---

## 7. 验收

- C1：plan 场景 1 用 `dagent run` 完成。
- C2：plan 场景 8（Ctrl+C 1 秒内退出、退出码 130、记录闭合）、9（jsonl 可解析且满足事件文法）；`dagent run … | head -c 10`
  不崩溃；`--output json` 的对象字段齐全。
- C3：plan 场景 12（sessions 列表、`--continue`、id 前缀）；`dagent trust` 之后项目级配置生效。
