# exec：子进程与沙箱

执行外部命令的模块。头文件在 `src/public/exec/`，实现在 `src/private/exec/`，构建为静态库
`dagent_exec`，命名空间 `dagent::exec`。依赖 base；内部使用 Boost.Process v2 + Asio（header-only）、
tree-sitter + tree-sitter-bash。

---

## 1. 概述

| 能力 | 头文件 | 主要接口 |
| --- | --- | --- |
| 一次性命令：超时、取消、流式输出、输出截断，结束时清理整个进程组 | `exec/process.hpp` | `run`、`which`、`shell_quote` |
| 长期存活的子进程：按行读 stdout，线程安全地写 stdin（给 MCP stdio 用） | `exec/child.hpp` | `Child` |
| bash 命令分析：语法状态、简单命令、影响证据和只读分类 | `exec/shell.hpp` | `analyze`、`is_known_readonly`、`is_dangerous` |
| 受限执行的中立值：模式、路径/网络边界与能力快照 | `exec/sandbox.hpp` | `Policy`、`Support`、`sensitive_paths` |
| SRT 后端：bridge 控制通道、运行中网络审批、只读搜索边界 | `exec/srt.hpp` | `run_srt`、`srt_config`、`ReadOnlySandbox` |

`exec/detail.hpp` 放的是模块内部几个实现文件共用的代码，外部不要 include。

---

## 2. 一次性命令：run

```cpp
exec::Command cmd{.argv = {"bash", "-c", command}, .cwd = workspace, .merge_stderr = true};
exec::Result r = exec::run(cmd, options,
                           [&](exec::Stream s, std::string_view data) { /* 流式输出 */ },
                           stop_token);
```

- **在调用线程上阻塞**，`on_output` 也在调用线程上触发，拿到的是未截断的原始数据。
- **不经过 shell**：argv 原样传给 exec。要执行 shell 命令，由调用方自己拼 `bash -c`，参数用 `shell_quote` 转义。
- **结果**：正常退出时 `exit_code` 有值，被信号杀死时 `signal` 有值，超时的话 `timed_out` 为 true。退出码非 0、超时都属于正常结果，不抛异常。
- **异常**：启动失败（命令不存在、没有执行权限、建管道失败）抛 `ExecError{spawn_failed}`；取消抛 `ExecError{cancelled}`，抛出时进程组已经清理完毕；沙箱准备失败抛 `ExecError{sandbox}`。回调里抛出的异常会原样传回调用方。
- **输出**：`Result::out/err` 分别按 `max_output_bytes` 截断，保留头部和尾部，截断规则和 `base::truncate_middle` 相同。`merge_stderr` 为 true 时 stderr 也写进 stdout 管道，两路的交错顺序保持不变。

### 子进程的运行环境

| 方面 | 行为 |
| --- | --- |
| 会话 | 子进程先 `setsid()`：成为新进程组的组长，**并且没有控制终端**。`sudo`、`ssh` 这类要打开 `/dev/tty` 的程序会直接失败，不会跟 TUI 抢键盘 |
| stdin | 没有 `stdin_data` 时接 `/dev/null`，不继承 agent 的 stdin |
| 额外描述符 | `Command::inherit_fds` 显式列出需要跨 exec 保留的 FD，所有权仍属于调用方；启动器登记白名单，仅在目标子进程中清除这些 FD 的 CLOEXEC |
| 环境变量 | `Command::inherit_env=true` 时继承 agent 环境并按 `env_deny` 过滤；false 时从空环境开始。两者随后注入非交互默认项、`Options::environment`，再应用 `env_unset` 和 `env_set`。托管命令使用 false 和托管 PATH；受限 bash 还把 HOME/XDG/TMP 指向私有目录，宿主凭据及 BASH_ENV/加载器/语言注入变量不会进入命令 |
| 查找程序 | argv[0] 带 `/` 时直接使用，相对路径**相对 `Command::cwd`**；否则按**子进程将看到的 PATH**（也就是叠加 `env_set` 之后的值）查找 |
| 信号 | SIGPIPE 在子进程里恢复为默认行为，所以 `yes \| head` 这类管道能正常结束；信号屏蔽清空（父进程为 sigwait 屏蔽的 SIGINT/SIGTERM 不会带进子进程，`timeout` 与 SIGTERM 清理照常生效） |
| exec 前失败 | fork 之后 chdir、应用沙箱或 `execve` 失败时，错误码经管道交回父进程，子进程直接 `_exit(127)`：不执行 agent 的 atexit 与静态析构，也不会把 fork 时复制来的 stdio 缓冲再写一遍 |

### 进程组的清理

- **超时或取消**：先对整个进程组发 SIGTERM，等 `kill_grace` 后再发 SIGKILL。
- **后台进程占着管道**：主进程退出后，最多再读 `drain_after_exit` 这么久，然后 SIGKILL 整个进程组、停止读取。这样 `server &` 这类命令不会让调用方一直卡住。
- **所有退出路径**（正常结束、超时、取消、回调抛异常）结束时，都会对整个进程组补发一次 SIGKILL。

### 对整个进程的影响：忽略 SIGPIPE

第一次调用 `run` 或 `Child::spawn` 时，如果 SIGPIPE 仍是默认处理，就把它设为忽略。否则子进程提前退出以后，
agent 再往它的 stdin 写数据，整个 agent 都会被 SIGPIPE 杀死。这个设置作用于整个 agent 进程：在它之后，
写已关闭的管道或 socket 会返回 EPIPE，不会再产生信号。

---

## 3. 长期存活的子进程：Child

```cpp
auto child = exec::Child::spawn(cmd, options);   // 同样 setsid、过滤环境变量；stdin 接管道
child->on_line([](std::string_view line) { … });  // stdout 每收到一行调用一次
child->on_exit([](std::optional<int> code, std::optional<int> signal) { … });
child->write(json_line + "\n");                    // 线程安全
child->terminate();                                // 析构时也会自动调用
```

- 内部有一个读取线程，`on_line` 和 `on_exit` 都在这个线程上触发。
- stderr 未注册处理器时写到 `base::logger("mcp")`；`on_stderr` 可接管逐行处理并接收此前的有限缓存，供 MCP bridge 启动握手使用。
- `write` 积压超过 8 MiB，或者进程已经退出时，数据会被丢弃，并记一条日志。
- `terminate`：先对进程组发 SIGTERM，等 `kill_grace` 后发 SIGKILL，然后等读取线程结束。
- `on_exit`：正常情况下 code 和 signal 恰好有一个有值；等待子进程失败（极少见，会记日志）时两者都为空。

---

## 4. 命令分析：analyze / is_known_readonly

`Analysis` 的当前版本是 2。tree-sitter-bash 只做纯解析，不执行命令替换、source、脚本、函数或插件。
结果保留完整原文，并分别记录：`SyntaxStatus`、错误源码范围、简单命令原文/范围/字面 argv、参数是否动态、
读写/特殊影响、整体动态标记和 cwd 是否失去确定性。语法错误与合法但动态的命令不再共用一个否决位。

变量与命令展开、环境赋值、控制流、子 shell、解释器、路径形式的程序及未知结构标记为动态；动态只表示
静态分析不完整，是否执行由权限模式和真实后端能力决定。重定向按每一项记录：输入是 read，普通输出是 write，
描述符复制/关闭是 special，字面量 `/dev/null` 不形成持久写入。解析失败在工具 prepare 阶段直接返回源码字节位置，
不会先执行前面的完整语句。

`is_known_readonly` 仅在语法有效、没有动态结构、没有写入/网络影响且每条命令均满足保守分类时成立：

| 命令 | 条件 |
| --- | --- |
| `cd` | 只有 bash 工具提供 workspace 根时可参与只读组合，目标必须是 workspace 本身或其子目录；HOME、`-`、区外路径拒绝 |
| `ls` `cat` `head` `tail` `wc` `grep` `pwd` `echo` | 无条件 |
| `rg` | 不能带 `--pre*`、`--hostname-bin`、`--search-zip`，也不能带含 `z` 的短选项（这些都会执行外部程序） |
| `find` | 不能带 `-exec` `-execdir` `-ok` `-okdir` `-delete` `-fls` `-fprint*` |
| `git` | 全局选项只允许 `--no-pager`/`-P`（`-c` 能注入 alias）；子命令只能是 `status` `log` `diff` `show`，并且不能带 `--output*`、`--ext-diff`、`--textconv`、`--open-files-in-pager`；`branch` 只放行列出分支的写法（带位置参数时必须配合 `--list`） |

命令名里带 `/` 的一律不放行。

`is_dangerous` 在同一分析结果上识别刻意保持很短的硬拦名单：`mkfs*`、`dd of=/dev/*` 和其它块设备写入、
对 `/`、HOME 或过浅绝对路径的 `rm -rf`、关机重启、对根/HOME 的递归 chmod/chown，以及 curl/wget pipe 到
sh/bash。它只向 agent Policy 提供判定，不执行命令。

**「只读」不等于「安全」**：仓库自己的 `.git/config`（比如 `core.fsmonitor`、`diff.external`）能让
`git status`、`git diff` 执行任意程序。所以核心自动放行只读命令时，仍然应该用 `read_only` 沙箱去执行。

---

## 5. 受限执行：SRT 后端

`Policy` 包含读/写允许范围、受保护读/写子路径、网络与本地 socket 开关和私有临时空间要求。
受限命令由 SRT 后端执行：每次 `run_srt` 调用（包括受限 bash 和 web 请求）启动一个 Node bridge，bridge 初始化
`@anthropic-ai/sandbox-runtime`、生成 bubblewrap 包装命令并管理代理出口；命令的
stdin/stdout/stderr 保持独立，网络判定经独立控制 FD 交回核心权限层（见 [agent 权限契约](agent.md#7-权限与沙箱)）。

`exec::srt_config` 把中立 `Policy` 编译为 SRT 配置：`denyRead` 从 `/` 开始，只显式重开系统路径、
工作区、实例私有 HOME 与工具链目录；`denyWrite` 在可写树内以启动时快照拒绝工作区里已有的
`.env*`、`*.pem`、`*.key`、私钥名、`.ssh` 和 `.gnupg`；`/tmp` 只重开实例私有目录。敏感路径扫描由
`exec::sensitive_paths` 提供，授权范围进入 tool_started，BashView 另保存命令的执行事实。受限命令的 `/proc`、`/dev`
由 SRT 以新挂载处理，`/sys` 不被根 deny 隐藏——这些都是平台例外，文档如实列出。

`Support` 是启动时真实做一次最小隔离启动后的结果：`backend` 为 `srt` 或 `none`，并提供
`read_only_ready()` / `workspace_ready()`。权限层只在对应 profile 真实满足时启用；能力不足时受限命令
不启动。符合策略的 Bash 请求可询问一次性 host access，由人工或符合条件的父模型审批；没有可用审批路由时不执行。
只读/规划上限不能借此突破，glob/grep 和 web_search/web_fetch 不提供宿主回退。
unrestricted 的 bash 使用 host；web 仍强制进入只读 SRT，由运行中网络闸门自动放行未被拒绝的目标。
`tools::detail::sandbox_request` 统一把核心 Grant 映射为 SRT 请求，web 传输不自行建立另一套权限规则。

### 启动与并发

- bridge 的 cwd 是每执行私有、位于所有可写根之外的控制目录，实际命令 cwd 是 workspace；
  私有 cwd 落在可写根内时该 profile 直接拒绝，不静默继续。
- bridge 启动握手报告协议版本、SRT 版本与准备环境身份；不匹配或超过 `startup_timeout_ms` 时受限
  启动失败并给出阶段。
- 控制 socketpair 使用 `SOCK_CLOEXEC`，bridge 端通过 `Command::inherit_fds` 显式保留；
  不在父进程清除 CLOEXEC，避免并发启动时将通道泄漏给其他进程，也不依赖启动器偶然保留描述符。
- 工作区保护遵循启动快照语义：运行中新建的同名敏感文件不在保护承诺内。
- 受限命令的私有 HOME 使用短临时路径，避免深层 Home 导致 SRT Unix socket 路径过长；bridge 初始化异常按结构化错误报告。

### 宿主前置

bubblewrap 需要宿主允许相应 namespace。对开启 AppArmor 非特权 userns 限制的主机，
CMake 为实际后端路径生成专用 `dagent.apparmor`，由管理员安装和加载，详见 [构建与安装](../guide/build.md)。
正常启动按后端可执行路径自动匹配，不依赖通用 `aa-exec -p linux-sandbox` 包装器。
`dagent sandbox status` 分开报告静态依赖、AppArmor 标签和真实启动结果；有依赖不等于可隔离执行。
2026-09-30 开发主机的默认启动结果见 [验收记录](../archive/2026-09-30-srt-permissions.md)，不代替其他主机的探测。

---

## 6. 依赖与构建

- Boost.Process v2 以 header-only 方式使用，需要定义 `BOOST_PROCESS_USE_STD_FS=1`（已在 `dagent_exec` 上设置）。
- `cmake/deps.cmake` 里：tree-sitter v0.27.0 通过 FetchContent 构建；tree-sitter-bash v0.25.1 只拉取源码，直接编译发布包里自带的 `parser.c`/`scanner.c`，不需要 tree-sitter CLI。受限执行依赖运行时的 `internal/sandbox` 环境（Node + SRT + bwrap/socat）。
- 根项目启用了 C 语言，用来编译 tree-sitter。
