# 02 exec：子进程与沙箱

库 `dagent_exec`，命名空间 `dagent::exec`。依赖 base。

| 子功能 | 头文件 | 里程碑 |
| --- | --- | --- |
| 一次性命令：`run` | `exec/process.hpp` | **M2**（workspace 的 search/context 依赖它，M2 最先做这个） |
| 长期存活的子进程：`Child` | `exec/child.hpp` | M4（给 MCP 的 stdio 传输用） |
| 命令分析：拆分命令、判断是否只读 | `exec/shell.hpp` | M5 |
| OS 隔离：Landlock + seccomp | `exec/sandbox.hpp` | M5 |

沙箱和子进程放在同一个模块，是因为沙箱挂在子进程启动流程的 `on_exec_setup` 钩子上，
`Command` 里也要带上沙箱配置，两者分开反而别扭。

## 依赖

| 用途 | 依赖 | 引入方式 |
| --- | --- | --- |
| 子进程 | **Boost.Process v2 + Boost.Asio**（Boost 1.83 系统自带） | header-only；`target_compile_definitions(dagent_exec PRIVATE BOOST_PROCESS_USE_STD_FS=1)`（本机没有编译好的 Boost.Filesystem，必须加） |
| 文件系统隔离 | Landlock（内核，本机 ABI 7） | glibc 没有封装函数，直接用 `syscall()`，结构体定义在 `<linux/landlock.h>` |
| 网络隔离 | **libseccomp**（Ubuntu 24.04 上是 2.5.5） | `apt install libseccomp-dev`，用 pkg-config 查找 |
| 命令解析 | **tree-sitter** + **tree-sitter-bash** 语法 | 都用 FetchContent 引入，写死 tag；两者都是纯 C |

本机已经验证过：Boost.Process v2 用 header-only 方式启动 `sh`，在 `on_exec_setup` 里新建进程组，0.5 秒后
对整组发 SIGKILL，读端正常收到 EOF，0.5 秒退出。

为什么不自己用 posix_spawn：同时读 stdout 和 stderr、等待超时、响应取消，这几件事要一起多路复用；用
Asio 写只要几行，自己写 poll 循环还得处理 pidfd 和 EINTR。备选的 reproc 不能接入 Asio。

---

## 1. 一次性命令（process.hpp）· M2

### 职责

执行一条命令：带超时、可取消、能流式拿到输出、输出有上限，结束时**连同所有子孙进程一起**清理干净。

不做：需要伪终端的交互式程序（vim、less 这类走 TUI 的 `Runtime::run_external`）。

### 接口草图

```cpp
namespace dagent::exec {

struct Options {                                        // dagent.json 的 "process" 段
    std::chrono::milliseconds default_timeout{300000};
    std::size_t max_output_bytes = 256 << 10;           // 每一路输出的上限，超出后保留头尾（base::truncate_middle）
    std::chrono::milliseconds kill_grace{2000};         // SIGTERM 之后等多久再 SIGKILL
    std::chrono::milliseconds drain_after_exit{200};    // 主进程退出后，最多再读多久管道
    std::vector<std::string> env_deny{"*KEY*", "*TOKEN*", "*SECRET*", "*PASSWORD*"};   // 通配符，见坑 8
};

struct Command {
    std::vector<std::string> argv;                      // argv[0] 按 PATH 查找；不经过 shell
    std::filesystem::path cwd;
    std::vector<std::pair<std::string, std::string>> env_set;
    std::vector<std::string> env_unset;
    std::optional<std::string> stdin_data;              // 为空时接 /dev/null
    std::optional<std::chrono::milliseconds> timeout;   // 为空时用 Options::default_timeout
    bool merge_stderr = false;                          // true 时 stderr 写进 stdout 管道，保持交错顺序
    const struct Prepared* sandbox = nullptr;           // M5：不为空时在子进程里应用沙箱
};

enum class Stream { out, err };

struct Result {
    std::optional<int> exit_code;   // 正常退出时有值
    std::optional<int> signal;      // 被信号杀死时有值
    bool timed_out = false;
    base::Truncated out, err;
    std::chrono::milliseconds elapsed;
};

Result run(const Command&, const Options&,
           const std::function<void(Stream, std::string_view)>& on_output = {},
           std::stop_token stop = {});   // 取消时抛 ExecError{cancelled}；抛出时进程组已经清理完

std::optional<std::filesystem::path> which(std::string_view name);
std::string shell_quote(std::string_view);   // 核心拼 bash -c 字符串时要用

class ExecError : public std::runtime_error { /* Kind: spawn_failed / cancelled / sandbox */ };
}
```

bash 工具的调用方式是 `argv = {"bash", "-c", 命令}`，用哪个 shell 由核心决定。

### 必须处理的坑

1. **用 `setsid()`，不要用 `setpgid()`。** 在 `on_exec_setup` 里调 `setsid()`：子进程成为新会话的首进程，同时拥有新的进程组，**并且没有控制终端**。这样 `sudo`、`ssh` 去 open `/dev/tty` 时会直接失败，而不是跟 TUI 抢键盘、然后卡在那里等输入。
2. **杀进程要杀整个组**：先发 `kill(-pgid, SIGTERM)`，等 `kill_grace`，再发 `kill(-pgid, SIGKILL)`。只杀 bash 的话，`npm test` 这类命令起的孙进程会变成孤儿继续运行。
3. **后台子进程会让管道一直不 EOF**。比如 `bash -c "server &"`：主进程已经退出了，后台进程还持有管道的写端，读端永远等不到 EOF。规则是：主进程退出之后，最多再读 `drain_after_exit` 这么久，然后杀掉整个进程组、停止读取。
4. **stdin 默认接 `/dev/null`**，写法是 `process_stdio{nullptr, …}`（v2 的 `nullptr` 对应空设备，已核对头文件）。千万不能继承 TUI 的 stdin：子进程会偷走按键，`cat`、`python` 这类程序也会一直等输入。
5. **输出截断用 `base::truncate_middle`**：保留头部和尾部，报错信息通常在尾部。`on_output` 回调拿到的仍然是完整的原始数据流，截断只作用于 `Result` 里的内容。
6. **取消和超时统一走 Asio**：在 `std::stop_callback` 里调 `asio::post(ctx, …)` 触发清理（`post` 是线程安全的），超时用 `steady_timer`。两条路径最终走同一段杀进程组的代码。
7. **默认注入的环境变量**：`PAGER=cat`、`GIT_PAGER=cat`、`GIT_TERMINAL_PROMPT=0`、`TERM=dumb`、`NO_COLOR=1`，防止命令进入分页器或者停下来等输入，也减少颜色转义序列。调用方可以用 `env_unset` 撤掉其中任意一个。
8. **过滤敏感的环境变量**：名字匹配 `env_deny` 的变量不传给子进程（codex 的 shell_environment_policy 也是这样做的）。用户 shell 里本来就有的 `OPENAI_API_KEY` 之类，不应该让模型一条 `env` 命令就看到。`env_set` 里显式传入的变量不受这个过滤影响。
9. 启动失败（命令不存在、没有执行权限）抛 `ExecError{spawn_failed}`；**退出码非 0 不算错误**。

---

## 2. 长期存活的子进程（child.hpp）· M4

给 MCP 的 stdio 传输使用：stdin 和 stdout 走管道，stderr 转到日志。

```cpp
namespace dagent::exec {
class Child {
public:
    static std::unique_ptr<Child> spawn(const Command&, const Options&);   // 同样调用 setsid 并过滤环境变量；stdin 接管道
    void write(std::string_view);                                         // 线程安全
    // 回调都在内部的读取线程上触发
    void on_line(std::function<void(std::string_view)>);                  // stdout 每收到一行调用一次
    void on_exit(std::function<void(std::optional<int> code, std::optional<int> signal)>);
    void terminate();                                                     // 先 SIGTERM 进程组，超时后 SIGKILL；析构时自动调用
};
}
```

实现方式：内部开一个 `std::jthread` 跑 Asio 的 `io_context`，按行切分用 `asio::async_read_until`，
stderr 的每一行写到 `base::logger("mcp")`。

---

## 3. 命令分析（shell.hpp）· M5

把一条 bash 命令解析成一组简单命令，判断它是不是「已知只读」的。核心的权限策略根据这个结果决定要不要
自动放行。权限策略本身（问不问用户、怎么问、授权记多久）属于核心。

为什么要用 tree-sitter：`ls && rm -rf x`、`echo $(curl …)`、`cat a | sh` 这样的命令，用字符串切分或者
正则来判断「是否只读」必然会漏。tree-sitter-bash 能给出完整的语法树，codex 也是这样做的。

```cpp
namespace dagent::exec {
struct SimpleCommand { std::vector<std::string> argv; };   // 已展开引号的字面量
struct Analysis {
    std::vector<SimpleCommand> commands;   // 由 && || ; | 连接起来的每一条简单命令
    bool has_opaque;                       // 含有 $(…)、反引号、写文件的重定向、eval、未知语法等无法静态判断的结构
};
Analysis analyze(std::string_view bash_source);
bool is_known_readonly(const Analysis&);   // 所有子命令都在只读白名单里，并且 has_opaque=false
}
```

**只读白名单要保守**：`ls cat head tail wc rg grep find（不带 -exec/-delete）git（只允许 status/log/diff/show/branch）`
这类命令。`find -exec`、`git -c core.pager=…`、`sed -i` 必须判为非只读。**白名单里的每一项都要附上理由。**

---

## 4. OS 隔离（sandbox.hpp）· M5

让 bash 工具启动的进程只能写工作区（以及 /tmp），默认禁止联网。即使模型或命令本身有恶意，也破坏不了
工作区以外的东西。

### 为什么不用 bubblewrap

本机实测：Ubuntu 24.04 默认开启 `kernel.apparmor_restrict_unprivileged_userns=1`（AppArmor 禁止没有
专门配置的普通程序创建 user namespace），`bwrap` 连启动都失败（`setting up uid map: Permission denied`）。
要解决只能装 AppArmor 配置或者改 sysctl，相当于要求每个用户都去改系统设置。

**Landlock** 是内核自带的机制，不需要任何特权，进程可以给自己（以及以后 exec 出来的程序）加上访问限制。
codex 在 Linux 上用的也是 Landlock + seccomp。

网络隔离用 seccomp，而不用 Landlock 自己的网络规则，原因是：Landlock 的网络规则（ABI ≥ 4）只管 TCP 的
bind 和 connect，**管不住 UDP，DNS 查询照样能发出去**。用 seccomp 直接拒绝 `socket(AF_INET/AF_INET6, …)`
更彻底，`AF_UNIX` 仍然放行。

### 接口草图

```cpp
namespace dagent::exec {
enum class Mode { read_only, workspace_write, full_access };   // 和 codex 的三档对应
struct Policy {
    Mode mode = Mode::workspace_write;
    std::vector<std::filesystem::path> writable;   // 默认：工作区根目录 + /tmp + /dev/null
    bool allow_network = false;
};

struct Prepared;                                    // 不透明类型：路径 fd、Landlock 规则集 fd、BPF 程序
std::unique_ptr<Prepared> prepare(const Policy&);  // 在父进程里执行
// apply_in_child 不对外公开：run() 发现 Command::sandbox 不为空时，在 on_exec_setup 里调用它

struct Support { int landlock_abi; bool seccomp; };
Support probe();                                    // 启动时探测一次，不支持时核心要降级为「必须询问用户」
}
```

### 必须处理的坑

1. **fork 之后、exec 之前，只能调用 async-signal-safe 的函数。** 多线程程序 fork 以后，子进程里只剩一个线程，这时调用 malloc 可能会死锁，而本项目的 TUI 就是多线程的。所以：路径的 `open(O_PATH)`、Landlock 规则集、seccomp 过滤器的生成，**都要在父进程里提前做完**。子进程只执行 `prctl(PR_SET_NO_NEW_PRIVS)`、`landlock_restrict_self`，以及加载事先生成好的 BPF 程序。seccomp 过滤器在父进程里用 `seccomp_export_bpf` 导出到一个 `memfd`，再读回成 `sock_filter` 数组（2.5.5 还没有 2.6 才加入的 `seccomp_export_bpf_mem`）。子进程里用 `prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)` 直接加载，**不要在子进程里调 `seccomp_load`**，它会分配内存。
2. **Landlock 的 ABI 要做降级兼容**：先用 `landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)` 查出当前 ABI，再把 `handled_access_fs` 里当前内核不支持的位去掉（ABI 2 才有 REFER，ABI 3 才有 TRUNCATE），否则规则集创建会失败。
3. **只读模式也要放行** `/dev/null`、`/proc/self` 等路径，否则很多程序会莫名其妙地失败。
4. **规则只对 exec 出来的程序生效**，会继承给它的所有子孙进程，而且无法撤销，这正是我们想要的效果。**但千万不能在 agent 自己的进程里调用 `restrict_self`。**

---

## 验收（temp/exec_check）

**process（M2）**

1. `echo` 能拿到输出；`sh -c 'exit 3'` 返回 exit_code=3；`sh -c 'kill -9 $$'` 返回 signal=9。
2. `merge_stderr` 打开时，stdout 和 stderr 交替输出的顺序保持不变。
3. **超时要杀干净子孙进程**：跑 `sh -c 'sleep 100 & sleep 100'`，设 1 秒超时。1 秒左右返回，之后用 `pgrep -f "sleep 100"` 查不到任何进程。
4. **后台进程不能让调用方卡住**：`sh -c 'sleep 100 &'` 在 `drain_after_exit` 之后很快返回。
5. **取消要快**：在另一个线程里 `request_stop()`，从发出到抛出异常，耗时在 10ms 量级。
6. `yes | head -c 10000000`：`total_bytes` 等于 1e7，保留了头尾，结果是合法的 UTF-8。
7. 不给 `stdin_data` 时，`cat` 立刻结束。
8. `sh -c 'echo x > /dev/tty'` 失败，原因是没有控制终端。
9. 在父进程里设置 `FOO_API_KEY=1` 后，子进程执行 `env` 的输出里看不到它；通过 `env_set` 显式传入的变量能看到。
10. 命令不存在时抛 `spawn_failed`。

**Child（M4）**

11. 启动 `cat`，写 3 行再读回 3 行；kill 掉子进程以后，`on_exit` 触发；析构时整个进程组被清理干净。

**命令分析与沙箱（M5）**

12. `ls && cat a | wc -l` 拆成 3 条简单命令，判为只读。`ls; rm -rf x`、`echo $(curl x)`、`cat a > b`、`find . -delete`、`git -c x=y status` 都判为非只读。引号、转义、heredoc 都能正确还原成 argv。
13. `workspace_write` 模式下：写工作区内的文件成功；写 `~/x`、`/etc/x` 失败，错误是 `Permission denied`；读 `/etc/os-release` 成功。
14. `allow_network=false` 时：`curl https://example.com` 和 `getent hosts example.com` 都失败（后者验证 UDP 也被拦住了）；`AF_UNIX` 通信正常。
15. 限制会继承给孙进程：`bash -c 'python3 -c "open(\"/etc/x\",\"w\")"'` 失败。`read_only` 模式下，连写 `/tmp` 也失败。
16. 连续执行 100 次沙箱命令以后，agent 进程本身没有被限制，fd 也没有泄漏（对比 `/proc/self/fd` 的数量）。

## 审核关注点

- process：坑 1–4 逐条对照；所有退出路径（正常、超时、取消、异常）都要走到清理进程组的代码。
- sandbox：子进程里有没有任何一次内存分配；ABI 降级的写法；只读白名单每一项的理由。
