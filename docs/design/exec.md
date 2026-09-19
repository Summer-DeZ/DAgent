# exec：子进程与沙箱

执行外部命令的模块。头文件在 `src/public/exec/`，实现在 `src/private/exec/`，构建为静态库
`dagent_exec`，命名空间 `dagent::exec`。依赖 base；内部使用 Boost.Process v2 + Asio（header-only）、
libseccomp、tree-sitter + tree-sitter-bash。

---

## 1. 概述

| 能力 | 头文件 | 主要接口 |
| --- | --- | --- |
| 一次性命令：超时、取消、流式输出、输出截断，结束时清理整个进程组 | `exec/process.hpp` | `run`、`which`、`shell_quote` |
| 长期存活的子进程：按行读 stdout，线程安全地写 stdin（给 MCP stdio 用） | `exec/child.hpp` | `Child` |
| bash 命令分析：拆出简单命令，判断是不是「已知只读」 | `exec/shell.hpp` | `analyze`、`is_known_readonly` |
| OS 隔离：Landlock 限制写文件，seccomp 禁止联网 | `exec/sandbox.hpp` | `prepare`、`probe` |

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
| 环境变量 | 继承 agent 的环境，但名字匹配 `env_deny` 的变量会被过滤掉（默认是 `*KEY*`、`*TOKEN*`、`*SECRET*`、`*PASSWORD*`，不区分大小写）；然后注入 `PAGER=cat`、`GIT_PAGER=cat`、`GIT_TERMINAL_PROMPT=0`、`TERM=dumb`、`NO_COLOR=1`；再应用 `env_unset`，最后叠加 `env_set`（`env_set` 不受过滤影响） |
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
- stderr 的每一行写到 `base::logger("mcp")`。
- `write` 积压超过 8 MiB，或者进程已经退出时，数据会被丢弃，并记一条日志。
- `terminate`：先对进程组发 SIGTERM，等 `kill_grace` 后发 SIGKILL，然后等读取线程结束。
- `on_exit`：正常情况下 code 和 signal 恰好有一个有值；等待子进程失败（极少见，会记日志）时两者都为空。

---

## 4. 命令分析：analyze / is_known_readonly

用 tree-sitter-bash 解析命令，把 `&&`、`||`、`;`、`|` 连起来的每一条简单命令拆出来，并把引号、转义、
heredoc 都还原成 argv。

下面这些结构无法静态判断，遇到时 `has_opaque = true`：命令替换和反引号、变量展开、**任何变量赋值**
（包括 `LD_PRELOAD=… cmd` 这种前缀赋值，以及单独一行的 `PATH=…`）、不带引号的花括号展开、写文件的重定向、
heredoc、控制流、子 shell，以及解析出错的地方。

`is_known_readonly` 要求 `has_opaque = false`，并且每条命令都在白名单里：

| 命令 | 条件 |
| --- | --- |
| `ls` `cat` `head` `tail` `wc` `grep` `pwd` `echo` | 无条件 |
| `rg` | 不能带 `--pre*`、`--hostname-bin`、`--search-zip`，也不能带含 `z` 的短选项（这些都会执行外部程序） |
| `find` | 不能带 `-exec` `-execdir` `-ok` `-okdir` `-delete` `-fls` `-fprint*` |
| `git` | 全局选项只允许 `--no-pager`/`-P`（`-c` 能注入 alias）；子命令只能是 `status` `log` `diff` `show`，并且不能带 `--output*`、`--ext-diff`、`--textconv`、`--open-files-in-pager`；`branch` 只放行列出分支的写法（带位置参数时必须配合 `--list`） |

命令名里带 `/` 的一律不放行。

**「只读」不等于「安全」**：仓库自己的 `.git/config`（比如 `core.fsmonitor`、`diff.external`）能让
`git status`、`git diff` 执行任意程序。所以核心自动放行只读命令时，仍然应该用 `read_only` 沙箱去执行。

---

## 5. OS 隔离：prepare / probe

```cpp
auto sandbox = exec::prepare({.mode = exec::Mode::workspace_write, .writable = {root, "/tmp"}});
cmd.sandbox = sandbox.get();      // run() 执行期间 sandbox 必须一直有效
```

| Mode | 可写路径 |
| --- | --- |
| `read_only` | 只有 `/dev/null`、`/proc/self` |
| `workspace_write` | `writable` 里列出的路径，再加 `/dev/null`；`writable` 为空时用 agent 的当前目录、`/tmp` 和 `/dev/null` |
| `full_access` | 不限制文件系统 |

- **文件系统**：用 Landlock 管理「写」类权限（写入、创建、删除、改名、截断……），读和执行不受限制。会按内核的 ABI 版本自动去掉当前内核不支持的权限位。
- **网络**（`allow_network = false` 时）：用 seccomp 拒绝创建 `AF_INET`/`AF_INET6` socket，TCP 和 UDP（包括 DNS）都会被拦住，`AF_UNIX` 正常可用；同时禁用 io_uring，防止通过 `IORING_OP_SOCKET` 绕过这条限制。
- **fork 安全**：规则集、BPF 程序都在父进程的 `prepare` 里准备好；子进程在 exec 之前只调 `prctl` 和 `syscall`，不做任何内存分配。限制只作用于 exec 出来的程序及其子孙，不影响 agent 本身。
- `probe()` 返回内核支持的 Landlock ABI 版本以及 seccomp 是否可用。不支持时，核心应该降级为「每条命令都询问用户」。

### 为什么不用 bubblewrap

Ubuntu 24.04 默认开启 `kernel.apparmor_restrict_unprivileged_userns=1`，没有专门 AppArmor 配置的程序
不能创建 user namespace，bwrap 连启动都失败。Landlock 不需要任何特权，codex 在 Linux 上用的也是
Landlock + seccomp 这套方案。

### 已知限制

- Landlock 的规则只能放行、不能拒绝，所以在 `workspace_write` 模式下，工作区里的 `.git` 同样可写，模型可以往 `.git/hooks` 里写脚本。
- 只做到「禁止写」和「禁止联网」，读取不受限制，`~/.ssh` 之类的文件在沙箱里仍然能读到。

---

## 6. 依赖与构建

- Boost.Process v2 以 header-only 方式使用，需要定义 `BOOST_PROCESS_USE_STD_FS=1`（已在 `dagent_exec` 上设置）。
- `cmake/deps.cmake` 里：tree-sitter v0.27.0 通过 FetchContent 构建；tree-sitter-bash v0.25.1 只拉取源码，直接编译发布包里自带的 `parser.c`/`scanner.c`，不需要 tree-sitter CLI；libseccomp 通过 pkg-config 查找（`apt install libseccomp-dev`）。
- 根项目启用了 C 语言，用来编译 tree-sitter。
