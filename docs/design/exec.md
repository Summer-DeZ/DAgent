# exec：子进程与沙箱

执行外部命令的模块。头文件在 `src/public/exec/`，实现在 `src/private/exec/`，构建为静态库
`dagent_exec`，命名空间 `dagent::exec`。依赖 base；内部使用 Boost.Process v2 + Asio（Linux 为 header-only，Windows 链接 Boost::process）、
tree-sitter + tree-sitter-bash；受限执行在运行时加载 MXC 原生库，不在链接期依赖它。

---

## 1. 概述

| 能力 | 头文件 | 主要接口 |
| --- | --- | --- |
| 一次性命令：超时、取消、流式输出、输出截断，结束时清理整个进程组 | `exec/process.hpp` | `run`、`which`、`shell_quote` |
| 平台工具路径与搜索路径默认值 | `exec/process.hpp` | `system_git_program`、`default_search_path`、`project_search_path` |
| 长期存活的宿主子进程：按行读 stdout，线程安全地写 stdin（SearXNG 服务使用） | `exec/child.hpp` | `Child` |
| bash 命令分析：语法状态、简单命令、影响证据和只读分类 | `exec/shell.hpp` | `analyze`、`is_known_readonly`、`is_dangerous` |
| 受限执行的中立值：模式、路径/网络边界与能力快照 | `exec/sandbox.hpp` | `Policy`、`Support`、`sensitive_paths` |
| MXC 后端：原生库加载与版本校验、策略编译、受限执行、长期受限子进程、只读搜索边界 | `exec/mxc.hpp` | `MxcLibrary`、`mxc_policy`、`allowlist_gate`、`run_mxc`、`MxcChild`、`ReadOnlySandbox` |
| MXC 1.0.1 的 C ABI：结构体布局、状态码与函数指针类型 | `exec/mxc_ffi.hpp` | `ffi::Functions`、`ffi::Status` |
| 每次受限执行独占的回环 HTTP 代理，按 host:port 调用网络闸门 | `exec/proxy.hpp` | `NetworkProxy` |

`exec/detail.hpp` 与 `exec/platform.hpp` 是模块内部接口，外部不要 include。
`platform_${DAGENT_PLATFORM}.cpp` 集中实现环境快照、变量名匹配、程序解析、工具和搜索路径默认值、原生管道、
两套启动钩子、进程组、退出状态，以及 MXC 后端用到的动态库句柄 `SharedLibrary`、沙箱命令行拼接
`sandbox_command_line` 和回环连接属主检查 `loopback_peer_is_current_user`；
`process.cpp`、`child.cpp` 保留 Asio 调度、计时器、取消、输出处理和回调，`mxc.cpp` 保留库加载、策略编译与执行编排，
`proxy.cpp` 实现代理协议与判定。
原生 PipeFds 只在平台实现内部定义，公共头不暴露 fd 或 PID。

Windows 的平台实现采用 UTF-16 环境块和 argv、分号 PATH、仅 `.exe`/`.com` 的 PATHEXT 解析，
变量名以 Windows ordinal 忽略大小写比较。共享合并顺序及重复项处理不变，生成环境块时稳定排序。
`environment_names_equal` 因转码可抛异常；共享查询按实际等号位置取值，避免 UTF-8 名称长度差异截错数据。
Windows deny 匹配支持 `*`、`?`、集合、否定集合、范围及转义，不支持 POSIX 命名字符类。

Windows 的两个 spawn 入口使用 Boost 1.92 launcher：stdio 的父端是 Asio overlapped 管道，子端只通过显式句柄清单继承；
无 stdin 数据时接 NUL，merge 模式共享 stdout 子端。挂起创建、新进程组、capture 加入独立 Job、恢复线程依次完成。
`ProcessGroup::capture` 可报告失败；加入 Job 或恢复线程失败即终止挂起进程并报告启动失败。
Job 设置 KILL_ON_JOB_CLOSE；terminate 尝试 Ctrl-Break，宽限到期 force_kill 使用 TerminateJobObject，
共享计时器、截断和输出回调不变。退出码保留 DWORD 的 32 位到 int（高位为 1 时为负数），signal 留空。
Windows 的 `SharedLibrary` 使用 `LoadLibraryW`/`GetProcAddress`，`sandbox_command_line` 按 CreateProcess 引用规则拼出
`"<shell>" --noprofile --norc -c "<script>"`，`loopback_peer_is_current_user` 第一版固定返回 true；
ProcessContainer 后端接入（S3）进行中，当前 Windows 装配不启用沙箱后端。
完整路径、继承与资源契约见 [platform](platform.md)。下面涉及 POSIX 系统调用的步骤描述 Linux 行为。

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
| 额外资源 | `Command::inherited_resources` 持有只读的不透明继承清单，不拥有 OS 资源；平台实现在 fork 前展开为 fd span 并登记白名单，仅在目标子进程中清除 CLOEXEC。除 stdio 与继承清单外的描述符在 exec 前全部关闭：Boost 1.83 的 `close_all_fds` 不会关闭白名单项之间的描述符，所以由 `ExecSetup` 自己按白名单用 `close_range` 关闭，日志文件和 curl 的 socket 不会进入子进程 |
| 环境变量 | `Command::inherit_env=true` 时继承 agent 环境并按 `env_deny` 过滤；false 时从空环境开始。两者随后注入非交互默认项、`Options::environment`，再应用 `env_unset` 和 `env_set`。托管命令使用 false 和托管 PATH；受限 bash 还把 HOME/XDG/TMP 及 uv/npm 缓存指向私有目录，宿主凭据及 BASH_ENV/加载器/语言注入变量不会进入命令 |
| 查找程序 | argv[0] 带 `/` 时直接使用，相对路径**相对 `Command::cwd`**；否则按**子进程将看到的 PATH**（也就是叠加 `env_set` 之后的值）查找 |
| 信号 | SIGPIPE 在子进程里恢复为默认行为，所以 `yes \| head` 这类管道能正常结束；信号屏蔽清空（父进程为 sigwait 屏蔽的 SIGINT/SIGTERM 不会带进子进程，`timeout` 与 SIGTERM 清理照常生效） |
| exec 前失败 | fork 之后 chdir 或 `execve` 失败时，错误码经管道交回父进程，子进程直接 `_exit(127)`：不执行 agent 的 atexit 与静态析构，也不会把 fork 时复制来的 stdio 缓冲再写一遍 |

`inherited_environment` 在父进程中复制原环境条目，返回有所有权的 `vector<string>`，保留顺序和重复项。
共享 `make_environment` 直接使用快照容器，稳定删除无名、无等号或匹配 env_deny 的继承项；
再按默认项 → `Options::environment` → `env_unset` → `env_set` 的顺序处理，不缓存或去重。
变量名相等由 `environment_names_equal` 判断，POSIX 区分大小写；赋值只覆盖第一个同名项，
`find_env` 返回第一个匹配值，unset 删除全部同名项。deny 通配符匹配独立使用 `environment_name_matches`，
保留 fnmatch/FNM_CASEFOLD。

`resolve_program` 接收完整环境；Runner 和 Child 传入合并后的子进程环境，`which` 传入原始宿主快照，
不应用默认项、deny 过滤或显式覆盖。POSIX 仍用 PATH 的第一个值，按 `:` 拆分，空分量表示 cwd；
整个 PATH 为空或缺失时不搜索裸程序名，带目录的路径不依赖 PATH。`regular_executable` 保留
stat/普通文件判断后再检查执行权限，解析不新增扩展名补全、路径规范化或宿主 PATH 回退。

`system_git_program` 在 POSIX 固定返回 `/usr/bin/git`，不探测、不缓存；`default_search_path` 固定为
`/usr/bin:/bin`，ReadOnlySandbox 仍在 command.env_set 之后、LC_ALL 之前追加该 PATH。
`project_search_path` 返回非空宿主 PATH，否则返回 `/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`；
该回退用于 project 受限 Bash，不用于普通程序解析。

`spawn_command` 独立保留原启动路径：环境指针表→按需 stdin 管道或 /dev/null→stdout→按需独立 stderr→
stdio 绑定→argv 与继承清单→Boost 启动→设置 spawned→保存进程号→关闭子端→按 stdout、stderr、stdin 顺序移交 Asio。
每次 close/assign 后仍在原位置将相应原生 fd 置 -1；merge 模式不创建和重复持有 stderr 端点。
管道创建及启动失败时由原 PipeFds 清理未移交端点；成功创建进程后的移交失败仍由 Runner 析构补发进程组 SIGKILL。

`spawn_child` 保留另一条路径：环境指针表→忽略默认 SIGPIPE→固定三条管道→argv 与继承清单→stdio 绑定→
Boost 启动→保存进程号→关闭 stdin 读端、stdout/stderr 写端→按 stdin、stdout、stderr 顺序移交 Asio。
随后共享 Child::Impl 注册读取、退出回调并启动线程。构造失败仍由未移交的 PipeFds、已构造的 Asio/Boost 成员清理，
不新增进程组终止步骤。两套钩子均保留继承端点清 CLOEXEC→setsid→SIGPIPE 复位→清信号屏蔽→chdir 的执行顺序，
随后关闭白名单外的描述符；`ExecSetup` 排在 stdio 绑定之后，关闭发生在 stdio 的 dup2 完成之后。

### 进程组的清理

- **超时或取消**：先对整个进程组发 SIGTERM，等 `kill_grace` 后再发 SIGKILL。
- **后台进程占着管道**：主进程退出后，最多再读 `drain_after_exit` 这么久，然后 SIGKILL 整个进程组、停止读取。这样 `server &` 这类命令不会让调用方一直卡住。
- **所有退出路径**（正常结束、超时、取消、回调抛异常）结束时，都会对整个进程组补发一次 SIGKILL。

`ProcessGroup` 只保存不透明进程标识，terminate/force_kill 保留 kill(-pid)；析构只释放元数据，
各调用点原来的 spawned 或有效 PID 判断、发送信号时机不变。退出状态仍在原等待成功位置读取，
平台函数把该快照转换为中立的 code/signal，结果组装不再解析原生等待状态。

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
- stderr 未注册处理器时写到 `base::logger("mcp")`；`on_stderr` 可接管逐行处理并接收此前的有限缓存（SearXNG 用它保留启动错误行）。
  MCP stdio server 不经 `Child`，而由第 5 节的 `MxcChild` 在沙箱内启动，接口相同。
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
`git status`、`git diff` 执行任意程序。所以核心自动放行只读命令时，仍然用 `read_only` 沙箱去执行。

---

## 5. 受限执行：MXC 后端

`Policy` 包含模式、读/写允许范围、受保护读/写子路径、读取例外、敏感名称开关和已批准的网络目标。
受限命令由 Microsoft Execution Containers（MXC）1.0.1 的原生库执行：后端进程在运行时加载
`libmxc_ffi.so`（Windows 为 `mxc_ffi.dll`），每次 `run_mxc`（受限 bash、web 请求、只读搜索与上下文、启动探测）
调用一次 `mxc_spawn_json`；MCP stdio server 用同一套资源的长期形式 `MxcChild`。执行路径上没有 Node、bridge 进程或 socat，
Linux 的隔离由 MXC 自己生成的 bubblewrap 命令落实。运行中的网络判定经闸门交回核心权限层
（见 [agent 权限契约](agent.md#7-权限与沙箱)）。

### 原生库与 C ABI

库文件来自 runtime 的 `mxc` 包：`runtime.json` 登记 npm 包 `@microsoft/mxc-sdk@1.0.1` 的 tgz 与 SHA-256，
包内 `libraries` 给出库的相对路径（Linux-x86_64 为 `bin/x64/libmxc_ffi.so`，Linux-aarch64 为 `bin/arm64/libmxc_ffi.so`，
Windows-x86_64 为 `bin/x64/mxc_ffi.dll`），装配层经 `Toolchain::library("mxc")` 取得绝对路径，详见 [toolchain](toolchain.md)。
二进制不进仓库，构建期不链接该库。

MXC 不发布 C 头文件，并声明其 C ABI 随库版本变化、不是稳定外部 ABI。`exec/mxc_ffi.hpp` 按上游
`src/ffi/mxc_ffi/src/{lib,streaming}.rs`（v1.0.1）手写结构体布局、状态码和函数指针类型。
`MxcLibrary::load` 经 `detail::SharedLibrary` 打开文件（POSIX 为 `dlopen(RTLD_NOW | RTLD_LOCAL)`，Windows 为 `LoadLibraryW`），
解析全部 27 个导出符号，再比较 `mxc_version()` 与 `kMxcVersion`（`1.0.1`）；打开失败、缺符号或版本不符都抛
`ExecError{sandbox}`，消息含文件名与原因，后端因此不可用。已加载的库以 `shared_ptr<const MxcLibrary>` 在进程内共享，
`platform_support()`、`available_backends()` 返回 `mxc_platform_support_json`、`mxc_available_backends_json` 的解析结果，
供 `dagent sandbox status` 输出。FFI 的并发契约是：同一个沙箱句柄上的调用由调用方串行化，stdin/stdout/stderr
三个流对象可分别在各自线程上使用，每个流句柄只有一个所有者。

`MxcRuntime` 是启动装配时解析一次的资源：库、containment（Linux `bubblewrap`，Windows `processcontainer`，
由 `app::detail::sandbox_containment` 按平台填写）、托管 bash 与托管 rg 的路径。

### 策略编译：mxc_policy

`exec::mxc_policy` 把中立 `Policy` 编译为 MXC 1.0.0 策略 JSON（`version` 固定为 `kMxcPolicyVersion` 即 `1.0.0`，
MXC 只接受精确字符串）。bash、web、MCP、只读搜索与探测共用这一个函数：

| 来源 | MXC 字段 | 规则 |
| --- | --- | --- |
| `readable`，加托管 bash 与 rg 所在目录 | `filesystem.readonlyPaths` | 落在任一可写根内的可读根被去掉：同一路径同时出现在只读与读写授权里时 MXC 以只读为准，可写树内的读取由读写挂载提供 |
| `writable`（`read_only` 模式视为空）与本次执行的私有 HOME | `filesystem.readwritePaths` | — |
| `protected_read`，以及 `protect_sensitive_names` 时各 `readable` 根下的敏感路径 | `filesystem.deniedPaths` | `read_exceptions` 中的路径从中剔除 |
| `protected_write`，以及 `protect_sensitive_names` 时各可写根下的敏感路径 | 追加到 `filesystem.readonlyPaths` | 只保留位于可写根内的条目，即嵌套在读写挂载里的只读挂载；可写树外的条目本就不可写，不借此授予读取 |
| `MxcRuntime::containment` | `containment` | 为 `processcontainer` 时另写 `fallback.allowDaclMutation=false` |
| 请求带网络闸门时的代理 URL | `network`、`runtimeConfig.networkProxy` | `egress.default=deny`、`ingress.default=deny`、`ingress.hostLoopback=deny`；没有闸门时省略 `network` 与 `runtimeConfig` |

所有路径先 `weakly_canonical` 再去重，只传已存在的路径，不制造占位挂载点。敏感路径由 `exec::sensitive_paths`
在每次执行开始时扫描：`.env`、`.env.*`、`*.pem`、`*.key`、`id_rsa*`、`id_ed25519*` 以及 `.ssh`、`.gnupg` 目录
（不进入这两个目录，不跟随符号链接；根本身是文件时只检查它的名字），因此保护是启动时快照，运行中新建的同名文件不在承诺内。
`mxc_policy` 不读取 `Policy` 的 `allow_network`、`allow_local_sockets`、`private_tmp`：`/tmp` 总是 MXC 的私有 tmpfs，
网络只由请求是否带闸门决定；这几个字段仍随 Grant 进入 tool_started 与 BashView。
`mxc_policy` 不生成 `process`，由执行函数补上 `commandLine`、`cwd`、`env` 和 `timeout`（固定 0，超时由 DAgent 自己落实）。

### 沙箱内看到的文件系统

MXC 的 bubblewrap 后端从拒绝一切开始：以只读方式绑定 `/bin`、`/sbin`、`/lib*`、`/usr/bin`、`/usr/sbin`、`/usr/lib*`、
`/usr/libexec`、`/usr/share`、`/etc` 及 `/run` 下的 DNS 解析目录，另建最小的 `/dev`、`/proc` 和每次执行全新的 tmpfs `/tmp`；
宿主 `$HOME`、`/root`、`/opt`、`/usr/local`、`/var`、`/sys`、`/run/user/<uid>` 等其余路径一律不可见，
之后才叠加上表的策略挂载。命令以当前用户身份运行。私有 HOME 作为读写挂载进入私有 `/tmp`，可写且宿主可见，`/tmp` 的其余部分随命令消失。

下列行为是 MXC 1.0.1 在本机真实运行得到的平台例外，与旧 SRT 实现不同：

| 情况 | 实际行为 |
| --- | --- |
| `deniedPaths` 中的目录（如 `.ssh`） | 挂一层空的**可写** tmpfs：看不到宿主内容，写入返回成功但随命令结束消失，宿主文件不受影响；不会像旧 SRT wrapper 那样得到 EROFS |
| `deniedPaths` 中的文件 | 读和写都得到 `Permission denied` |
| 可读工作区根下的 `.env` 等敏感文件 | 进入 `deniedPaths`，即使工作区同时可写也不可读、不可写；只授予可写、未授予可读的根下才表现为嵌套只读（可读，写入 EROFS） |
| `protected_write`（如 `.git/config`、Home 的 `runtime/`）位于可写根内 | 可读，写入 `Read-only file system`（EROFS） |
| `/sys` | 不存在 |
| 命令 cwd 不在任何可见路径内 | bubblewrap 报 `Can't chdir` 并以 1 退出，MXC 只给出告警 |

### 一次执行：run_mxc

`run_mxc` 的语义与 `run` 一致：在调用线程上阻塞，`on_output` 也在调用线程上触发；超时置 `timed_out`，
取消抛 `ExecError{cancelled}`，后端或策略失败抛 `ExecError{sandbox}`，回调里抛出的异常原样传回调用方；
返回前进程树、代理和私有目录都已清理。一次执行依次：

1. 用 `base::create_unique_directory(base::default_temporary_directory() / "dagent-home-XXXXXX")` 建私有 HOME
   （POSIX 固定在 `/tmp`，不读取宿主 TMPDIR），并建 `config/`、`cache/`、`run/` 子目录。
2. 请求带闸门时启动本次执行独占的 `NetworkProxy`（见下文）。
3. 编译策略并补 `process`：`commandLine` 由 `detail::sandbox_command_line` 生成，POSIX 为
   `exec '<托管 bash>' --noprofile --norc -c '<脚本>'`（sh 单引号转义，由 MXC 的 `/bin/sh -c` 解释）；
   `merge_stderr` 为 true 时脚本前置 `exec 2>&1`，stderr 在 shell 内并入 stdout，交错顺序与宿主执行一致，
   shell 启动前后端自己写到 stderr 的诊断也归入 stdout。`cwd` 是请求的 workspace。
4. `env` 由 `detail::make_environment` 以 `inherit_env=false` 生成：非交互默认项 → `Options::environment` →
   删除 `BASH_ENV`、`ENV`、`BASHOPTS`、`SHELLOPTS`、`CDPATH`、`GLOBIGNORE`、`PROMPT_COMMAND`、`LD_PRELOAD`、`LD_LIBRARY_PATH`、
   `PYTHONPATH`、`PERL5LIB`、`RUBYOPT`、`NODE_OPTIONS`、`NODE_PATH`、`NODE_EXTRA_CA_CERTS` 与大小写两种 `*_proxy` →
   请求的环境（PATH、LC_ALL 等）→ HOME、`XDG_CONFIG_HOME`、`XDG_CACHE_HOME`、`UV_CACHE_DIR`、`NPM_CONFIG_CACHE`、
   `XDG_RUNTIME_DIR`、`TMPDIR`/`TMP`/`TEMP` 指向私有 HOME 内。MXC 以 `--clearenv` 启动，沙箱只看到这份列表；
   代理模式下 MXC 另行注入代理变量。
5. `mxc_spawn_json` 失败时把 MXC 的错误消息与 remediation 拼进 `ExecError{sandbox}`；成功后取出告警写 debug 日志。
6. 不需要 stdin 时立即释放 stdin 流，子进程读到 EOF（等同 `/dev/null`）；stdout、stderr 各有一个读取线程，
   以 64 KiB 缓冲阻塞调用 `mxc_stream_read`。调用线程约每 10 ms 轮询一次 `mxc_sandbox_try_wait`，按到达顺序派发输出，
   并在这里落实超时与取消。
7. 进程退出后最多再读 `drain_after_exit`，然后清理：仍在运行就杀进程树 → 关闭流的 closer 让阻塞中的读以 EOF 返回 →
   join 读取线程并释放流 → 释放 stdin → 必要时 `mxc_sandbox_wait` → `mxc_sandbox_free` → 停止代理 → 删除私有 HOME。

| 方面 | 行为 |
| --- | --- |
| 超时 | 限值取请求的 timeout，否则 `Options::default_timeout`；到期调用 `mxc_sandbox_kill_for_timeout`，`Result::timed_out=true`，`exit_code` 为空 |
| 取消 | stop_token 生效后调用 `mxc_sandbox_kill`，抛 `ExecError{cancelled}`（`command cancelled`）；本机实测从请求停止到返回约 11 ms |
| 终止方式 | MXC 对首进程发 SIGKILL，再杀整个进程组；没有 SIGTERM 宽限，`Options::kill_grace` 对 MXC 路径不生效 |
| 退出状态 | `Result::signal` 恒为空；沙箱内进程被信号终止时以退出码 128+N 报告 |
| 输出 | `out`/`err` 按 `max_output_bytes` 保留头尾，规则与 `run` 相同；`merge_stderr` 时 `err` 为空 |
| 启动开销 | 无网络 10–30 ms；代理模式 170–210 ms（2026-10-22 本机以 `true` 测得） |

### 网络：无网络与代理模式

网络只有两种姿态。请求没有闸门时策略省略 `network`，MXC 按无规则的 egress deny 只加 `--unshare-net`：
命令只有自己的 loopback，不启动 slirp4netns。请求带闸门时进入代理模式：

- DAgent 为本次执行启动一个 `exec::NetworkProxy`（`exec/proxy.hpp`、`proxy.cpp`，Asio 服务线程），监听 `127.0.0.1` 的内核分配端口，
  把 `http://127.0.0.1:<port>` 写进 `runtimeConfig.networkProxy`。
- MXC 用 slirp4netns 给沙箱一个私有网络命名空间，在其中以 iptables（`iptables-restore`）装默认 DROP 的出站链，只放行 loopback
  和经 slirp 网关 `10.0.2.2:<port>` 到达的代理端点，入站链同样默认 DROP；并用 `bwrap --setenv` 注入大小写两种
  `HTTP_PROXY`、`HTTPS_PROXY`、`ALL_PROXY`、`FTP_PROXY`，不设 `NO_PROXY`。不经代理的直连没有 DNS 和路由，访问宿主其它端口超时。
- 代理接受 `CONNECT host:port` 隧道（TLS 端到端，不解密）和绝对形式的明文请求 `GET http://host[:port]/path`
  （改写为 origin 形式转发，去掉 `Proxy-Connection` 头与 URL 中的用户信息）；其它请求或超过 64 KiB 的请求头回 400。
- 判定以小写 `host:port`（IPv6 加方括号）为键：允许结果在代理生命周期（即本次执行）内缓存，拒绝不缓存；
  同一目标的并发请求只提问一次。`policy.network_targets` 经 `allowlist_gate` 直接放行，不向外层闸门提问。
- 预算：向闸门提问（含 allowlist 判定）的次数超过 `max_network_requests_per_execution` 后一律拒绝（`network request budget exhausted`）。
- 审批超时：等待超过 `network_approval_timeout_ms` 视为拒绝（`network approval timed out`），本次执行之后不再提问，
  仍在等待的闸门经 stop_token 释放。
- 拒绝回 `403 Forbidden`，正文写明目标与原因；闸门返回 cancel（或抛异常）时该请求回 403，代理通知执行函数杀掉命令，
  `run_mxc` 抛 `ExecError{cancelled}`（`command cancelled by a runtime permission decision`）。
- 放行后才在宿主解析主机名（闸门拿到的是请求里写的名字），按解析结果依次连接；后面还有地址时每个地址最多等 5 秒，
  解析或连接失败回 502。请求头之后已读入的字节（请求体或提前发出的 TLS 数据）原样转发。
- 代理只接受当前用户的连接：Linux 的 `detail::loopback_peer_is_current_user` 在 `/proc/net/tcp` 中按对端端口找到连接条目，
  比较其 uid 与 `getuid()`（slirp4netns 以当前用户运行），不符回 403 并关闭；Windows 第一版固定返回 true。
  不用口令的原因是 MXC 拒绝带凭据的代理 URL：URL 经 `bwrap --setenv` 进入命令行参数，`/proc/<pid>/cmdline` 对所有本地用户可见。
- `NetworkProxy::stop` 关闭监听与全部隧道并等待服务线程；仍在等待闸门的请求按拒绝结束。

代理模式要求宿主具备 slirp4netns、util-linux 的 `unshare`（支持 `--map-current-user`、`--keep-caps`）与 `nsenter`、iproute2 的 `ip`、
nf_tables 后端的 iptables/ip6tables 与对应 `-restore`，并已加载 `nf_conntrack` 内核模块（非特权 bubblewrap 无法按需加载）；
MXC 在启动前校验这些前置，缺失即失败，从不退回共享宿主网络。

各调用方的网络姿态：

| 调用方 | 闸门 | 结果 |
| --- | --- | --- |
| 受限 bash | `tools::detail::sandbox_request` 总是安装，内部转调 Grant 的 `network_decider`；没有 decider 时拒绝并说明 `runtime network approval is unavailable in this run` | 每次受限 bash 都是代理模式 |
| web_search / web_fetch | 同上，web 再按调用包一层（托管端点预授权、逐跳审阅） | 代理模式；curl 带 `--noproxy ''`，连 SearXNG 的 `127.0.0.1:<port>` 也经代理 |
| MCP stdio | profile 有网络目标时为 `allowlist_gate(targets)`，没有运行中审批 | 有目标为代理模式，否则无网络 |
| `ReadOnlySandbox`（rg 搜索、上下文 Git） | 无 | 无网络 |
| 启动探测 | `filesystem` 阶段无，`network` 阶段为拒绝一切的闸门 | 依次覆盖两种姿态 |

### 长期受限子进程：MxcChild

`MxcChild::spawn(MxcRequest, Options)` 以与 `run_mxc` 相同的步骤启动受限子进程，但保留 stdin 管道；接口与 `exec::Child` 一致：

- `write` 线程安全，由写线程调用 `mxc_stream_write` 与 `mxc_stream_flush`；积压超过 8 MiB 或写失败后丢弃数据并记日志。
- `on_line` 按行回调 stdout（去掉行尾 `\r`）；`on_stderr` 按行接管 stderr，注册前到达的行写入 `base::logger("mcp")` 并最多缓存
  256 行，注册时回放。回调都在读取线程上触发。
- stdout 结束即视为进程结束（沙箱的 PID 命名空间随首进程退出），随后轮询退出码，最多等 `drain_after_exit` 让 stderr 收尾，
  再调用一次 `on_exit(code, nullopt)`；signal 恒为空。
- `terminate` 可重复调用：杀进程树、关闭 stdin、等待写线程，再按 `run_mxc` 的清理顺序释放流、沙箱、代理与私有 HOME；
  被杀的进程由 MXC 报告退出码 -1。

MCP stdio 传输的用法见 [mcp](mcp.md#stdio)。

### 只读执行边界：ReadOnlySandbox

`ReadOnlySandbox{runtime, protected_read, read_exceptions}` 供 workspace 的 rg 搜索与上下文 Git 使用。`run(Command)` 以
`read_only` 模式执行：可读范围只有 `command.cwd`（加托管 bash、rg 所在目录），argv 逐个 `shell_quote` 后拼成脚本，
环境为 `command.env_set` 加 `PATH=default_search_path()` 与 `LC_ALL=C.UTF-8`，不带闸门、不合并 stderr。
`runtime` 为空时抛 `ExecError{sandbox}`（`read-only sandbox execution unavailable; run dagent sandbox status`），没有宿主回退。
装配层把 Home 的 `config/`、`data/`、`logs/`、`run/` 与 `memory/` 放进它的 `protected_read`。

### 装配、能力快照与探测

`app::sandbox_runtime` 依次要求：本平台提供后端、runtime 已为当前配置准备好、`mxc` 库已安装、库可加载且版本为 1.0.1、
托管 bash 与 rg 可执行；任一条件不满足时返回说明原因的字符串。`app::sandbox_probe` 随后真实启动两次，均为 `read_only` 模式、
工作目录为托管 bash 所在目录、超时 30 秒：

| 阶段 | 命令 | 覆盖 |
| --- | --- | --- |
| `filesystem` | `printf ok > "$HOME/probe.txt" && cat "$HOME/probe.txt"`，输出须为 `ok` | 库调用、bubblewrap 命名空间、私有 HOME 读写 |
| `network` | `true`，闸门拒绝一切 | slirp4netns、命名空间内防火墙与代理端点 |

成功返回 `{ok:true, stage:"completed", mxc:<版本>, elapsed_ms:{filesystem, network}}`，失败返回 `ok:false` 及 `stage`、`error`、
`exit_code`、`timed_out`（执行层抛异常时只有 `stage` 与 `error`）。

交互与 run 模式每次启动后端（`app/bootstrap.cpp` 的会话装配）都执行一次 `sandbox_runtime` 与 `sandbox_probe`，
本机两阶段分别约 30 ms 与 200 ms；查询模式（sessions、`--list-models`）不探测。
成功时 `Support.backend` 为 `mxc`、`child_signals` 为 true，`MxcRuntime` 交给 `tools::Options::mxc` 与只读搜索；
失败时 `backend` 保持 `none`，`Support.missing` 记录 `sandbox_runtime` 的原因或 `MXC isolation probe failed at <stage> stage: <error>`。
`read_only_ready()` 与 `workspace_ready()` 都只判断 `backend != "none"`；能力不足时的模式映射与 host access 规则不变：
受限命令不启动，符合策略的 Bash 请求可询问一次性 host access，由人工或符合条件的父模型审批；没有可用审批路由时不执行。
沙箱不可用时，普通 Bash 请求改记为 `host_command`（命令直接在宿主以用户账户完整权限运行），
它与 `host_access` 不同：已知只读命令可会话级精确复用，敏感/控制面路径仍叠加一次性 `sensitive_read`。
只读/规划上限不能借此突破，glob/grep 和 web_search/web_fetch 不提供宿主回退。
unrestricted 的 bash 使用 host；web 仍强制进入只读 MXC 沙箱，由运行中网络闸门自动放行未被拒绝的目标。

受限 profile 的 Grant 的 backend 为 `mxc`，full_access 为 `host`。`tools::detail::sandbox_request` 统一把 Grant 映射为
`MxcRequest`（backend 不是 `mxc` 或 profile 为 full_access 时抛错），审批超时与每执行请求预算取自 `sandbox` 配置段；
web 传输不自行建立另一套权限规则。

### 宿主前置

Linux 上 MXC 的 bubblewrap 后端要求：bwrap ≥ 0.5（MXC 用到 `--clearenv`）、允许非特权 user namespace、
上文代理模式所列的网络工具与 `nf_conntrack`；预编译的 `libmxc_ffi.so` 引用 GLIBC_2.34 符号，需要 glibc ≥ 2.34。
bwrap、unshare、slirp4netns 都由 `dagent-backend` 进程内加载的库直接生成，对开启 AppArmor 非特权 userns 限制的主机，
CMake 仅在 Linux 构建和安装时为实际后端路径生成专用 `dagent.apparmor`（`flags=(unconfined)` 加 `userns`），子进程继承该 profile；
由管理员安装和加载，详见 [构建与安装](../guide/build.md)。正常启动按后端可执行路径自动匹配，不依赖 `aa-exec` 包装器。
`dagent sandbox status` 分开报告静态检查、MXC 自己的平台诊断和真实启动结果；静态项齐全不等于可隔离执行。

Windows 的 `sandbox_status_win.cpp` 目前令 `platform_sandbox_supported()` 返回 false，装配不加载库也不探测，
状态报告 `probe.stage="unsupported"`；containment `processcontainer`、`LoadLibraryW` 加载、Windows 命令行拼接和
`fallback.allowDaclMutation=false` 已在共享代码中。ProcessContainer 接入（S3）进行中：按已批准的计划只接受 `base-container` 层，
在 Windows 11 26H2 虚拟机上验收。

---

## 6. 依赖与构建

- Boost.Process v2 以 header-only 方式使用，需要定义 `BOOST_PROCESS_USE_STD_FS=1`（已在 `dagent_exec` 上设置）。
- `cmake/deps.cmake` 里：tree-sitter v0.27.0 通过 FetchContent 构建；tree-sitter-bash v0.25.1 只拉取源码，直接编译发布包里自带的 `parser.c`/`scanner.c`，不需要 tree-sitter CLI。
- MXC 原生库不在链接期依赖，也没有头文件：Linux 的 `dagent_exec` 只额外链接 `${CMAKE_DL_LIBS}` 以使用 `dlopen`；
  库文件由 `dagent runtime sync` 按 `runtime.json` 的 `mxc` 包下载并校验。代理使用 Boost.Asio。
- 根项目启用了 C 语言，用来编译 tree-sitter。
