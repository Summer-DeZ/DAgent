# 平台实现约定

平台源文件由根 CMake 的 `DAGENT_PLATFORM` 选择，当前值为 `posix`。`src/CMakeLists.txt`
显式列出 `private/<模块>/<文件>_${DAGENT_PLATFORM}.cpp`，不使用运行时平台选择或虚函数端口。
根 CMake 仍拒绝非 Linux 系统。

## 边界

- I1：自有 C++ 中的平台头文件和原生平台操作集中到 `src/private/**/*_posix.cpp`。
  范围包括 unistd/fcntl/poll/signal/termios/spawn/pthread、sys/linux 头文件，进程与信号、fd I/O、
  socketpair、文件锁、原生文件和目录操作、终端、时间转换、uname、监听及 `/proc` 路径。
  第三方 `src/public/lib/**`、SRT 资源脚本和 bash 语义分析字面量不受此限制；
  Boost.Asio/Process 的跨平台用法可以留在共享实现，POSIX 钩子与 fd 操作属于平台实现。
- I2：`src/public/**`（除 lib）不直接包含平台头文件，不在接口或成员中暴露 fd、pid_t、
  termios、sigset_t 等原生类型；资源状态用不透明类型表示。标准库类型可用于中立接口。
- 平台提取保留原操作、标志、顺序、错误和日志文本、超时、权限与参数；格式化和业务失败策略留在调用方。

这是逐步落实的源码边界约定。当前已提取跨模块小原语、workspace 文件操作与 OS 描述、入口与宿主辅助、IPC、TUI、exec/SRT 和存储平台操作，尚未完成全仓 I1/I2：
配置/凭据锁、监听与 sandbox status 等仍有共享实现中的平台代码；
ModelWriteLock 仍含原生状态。下面只列已落地的平台文件。

## 当前平台文件与接口

| 平台文件 | 中立声明 | 实现 |
| --- | --- | --- |
| `src/private/base/platform_posix.cpp` | `src/public/base/platform.hpp` | `executable_path`、`home_directory`、`process_id`、`stdin_is_terminal`、`stdout_is_terminal`、`local_time`、`utc_time`、`utc_timestamp`、`can_execute`、`create_unique_directory` |
| `src/private/workspace/context_posix.cpp` | `src/public/workspace/context.hpp` | `detail::os_description` |
| `src/private/app/cli_posix.cpp` | `src/public/app/cli.hpp` | `detail::read_stdin_all` |
| `src/private/app/paths_posix.cpp` | `src/public/app/paths.hpp` | `detail::executable_home_directory`、`detail::probe_home_writable` |
| `src/private/app/interrupts_posix.cpp` | `src/public/app/interrupts.hpp` | `install_interrupts`、`ignore_broken_pipe` |
| `src/private/app/backend_main_posix.cpp` | `src/public/app/interrupts.hpp` | `initialize_backend_signals` |
| `src/private/app/toolchain_posix.cpp` | `src/public/app/toolchain_detail.hpp` | `detail::runtime_platform`、`detail::SyncLock` |
| `src/private/ui/browser_posix.cpp` | `src/public/ui/browser.hpp` | `open_browser` |
| `src/private/web/document_posix.cpp` | `src/public/web/detail.hpp` | `detail::convert_charset` |
| `src/private/ipc/channel_posix.cpp` | `src/public/ipc/channel.hpp` | `InheritedEndpoint` 参数解析、`Channel` 原生状态与传输、`BackendProcess` 不透明进程标识、`spawn_backend`、`wait_or_terminate` |
| `src/private/tui/terminal_posix.cpp` | `src/public/tui/terminal.hpp`、`src/public/tui/event_wait.hpp` | Terminal 生命周期、原生状态、尺寸、字节写入、输入恢复与信号；EventWait 的等待、输入读取、唤醒及排空 |
| `src/private/exec/platform_posix.cpp` | `src/public/exec/detail.hpp`、`src/public/exec/platform.hpp`、`src/public/exec/process.hpp` | 环境枚举/匹配、执行权限、SIGPIPE、ProcessGroup、退出状态、spawn_command/spawn_child、InheritedResources、SrtControl、默认 SRT 可读目录 |
| `src/private/workspace/files_posix.cpp` | `src/public/workspace/files.hpp` | `probe`、`read_text`、`write_text`、`stamp_of`、FileReference 原生状态与操作 |
| `src/private/storage/storage_posix.cpp` | `src/public/storage/platform.hpp` | `detail::SessionFileLock` |
| `src/private/storage/decision_store_posix.cpp` | `src/public/storage/platform.hpp` | `detail::make_decision_file_private` |

`executable_path` 的两个重载分别保留 filesystem 的抛异常和 error_code 行为，供 IPC、sandbox status、
SearXNG 定位程序资源。安装根的固定缓冲区 readlink 和 InstallError 在 `app/paths_posix.cpp` 中保持独立；
`app/paths.cpp` 保留 DAGENT_HOME/dev 默认值选择和目录验证，可写探针仍使用原 O_EXCL、0600 与关闭/删除顺序。

`home_directory` 借用 getenv 的结果，不缓存、不规范化，保留 nullptr 与空字符串的区别。
`process_id` 返回当前进程号，调用位置与十进制格式不变。时间转换写入调用方的 `std::tm`，
local/UTC 转换返回是否成功，UTC 时间戳转换保留原返回值和对 tm 的修改；调用者维持各自的失败处理。

`can_execute` 只检查执行权限，不附加普通文件判断；exec 的 stat 检查由 `regular_executable` 在平台实现中完成。
`create_unique_directory` 接收完整 XXXXXX 模板，只调用 mkdtemp，成功返回目录，失败返回 strerror 文本。
父目录创建、固定 `/tmp` 选择、业务异常及清理由 SRT、MCP、SearXNG 各自保留。

`os_description` 返回 uname 的 `sysname release`，失败为 `unknown`。Toolchain 的 `runtime_platform`
保留 `sysname-machine` 与原异常。`SyncLock` 用 pimpl 保存 fd，仍非阻塞加独占锁，析构只 close，
锁范围仍覆盖原 sync 流程；文件锁未合并为 base 原语。

CLI 用 `stdin_is_terminal` 判断输入来源，用 `read_stdin_all` 读取管道输入，保留 EINTR 重试与其他错误时停止读取。
Terminal 也通过 base 判断 stdin/stdout 是否终端。前端先安装中断处理，再忽略断管信号；后端参数校验后恢复信号屏蔽并忽略断管信号，
随后接管 Channel。后端 `--ipc-fd` 保留原参数格式，参数值经不透明的 `InheritedEndpoint` 解析和接管。

浏览器启动完整实现在 `browser_posix.cpp`，保留原 posix_spawn 属性与回收线程。
网页 charset/BOM 检测留在共享文件，只有 iconv 转换循环进入 `document_posix.cpp`，保留缓冲区、截断与错误处理。

IPC 的共享文件只保留分帧、缓冲、错误状态与后端路径。Channel 和继承参数对象共用不透明端点状态，
参数解析与替换不取得连接所有权，adopt 移交已有状态而不重新分配。socketpair 的端点状态和后端进程状态
分别在创建连接、fork 前分配。BackendProcess 移动时保留源对象的进程标识，各对象的回收标记独立，
析构只关闭 channel；原生调用、flags、继承顺序和回收节奏见 [protocol](protocol.md#1-进程与传输)。

Terminal 的 Native 与 EventWait 的 Impl 都只在 `terminal_posix.cpp` 定义，公共头不包含终端类型或原生句柄。
EventWait 只报告就绪状态，Runtime 仍按信号、wake、stdin 的顺序消费，再执行 inbox、尺寸、定时器、Esc、握手和渲染。
`wake_pending_` 留在 Runtime，清除后才排空 wake pipe；self-pipe 创建失败降级，wake pipe 创建失败 terminate。
Terminal 的恢复与信号卸载顺序见 [tui-framework](tui-framework.md#41-终端l1)，构造所需状态在对应管道创建前分配。

exec 的跨平台 Asio/Boost.Process 对象和调度保留在共享文件，POSIX 启动钩子、原生管道与继承清单展开位于平台文件。
两条 spawn 路径独立，原创建、标记、关闭、移交顺序见 [exec](exec.md)。ProcessGroup 状态在 Runner/Child::Impl
构造阶段分配，早于创建子进程管道；其析构不发送信号。继承清单只保存元数据，不拥有资源，fork 前借用其中的 fd span。

SrtControl 用 optional 延迟到原 socketpair 位置构造，不把连接创建提前到目录创建之前；其 Impl 在 socketpair 前分配。
设置 Command 继承清单时，在已有控制连接上分配共享元数据及原有 fd 列表存储，均在 fork 前；失败走原资源清理路径。
环境枚举使用借用 span，不新增值复制或容器分配。fork 后钩子不展开不透明对象、不新增分配。
SRT 配置 JSON、控制帧和线程编排保持共享，各控制 I/O 方法保留原短写、EINTR、EPIPE 与关闭语义。

workspace 的路径和文本编解码通过 `files_detail.hpp` 供平台文件调用，不暴露原生句柄或 stat。
两条写入路径保持独立：通用 write_text 保留 `07777`、显式 close 后 rename；FileReference::write 保留 `0777`，
文件句柄保持打开直到 renameat2、同步目录和读取返回 Stamp 后清理。两者共用原 umask 静态缓存，
零字节写处理与异常清理顺序见 [workspace](workspace.md#2-文件原语fileshpp)。

SessionFileLock 的 Impl 在保存会话 ID 后、open 锁文件前分配；同进程租约注册表与互斥范围仍在 storage.cpp。
锁保持非阻塞、无 EINTR 重试，最后共享 State 销毁时 UNLOCK+close。决策库 chmod(0600) 仍忽略返回值，
不与其他模块的锁或权限策略合并，详见 [storage](storage.md#4-写所有权)。
