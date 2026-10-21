# 平台实现约定

平台源文件由根 CMake 根据 `CMAKE_SYSTEM_NAME` 设置的 `DAGENT_PLATFORM` 选择：Linux 为 `posix`，
Windows 为 `win`，其他系统以 `Unsupported target system` 错误拒绝配置。`src/CMakeLists.txt`
显式列出 `private/<模块>/<文件>_${DAGENT_PLATFORM}.cpp`，不使用运行时平台选择或虚函数端口。
当前仅包含 POSIX 实现；Windows 在 `project()` 完成工具链与编译器识别后、项目依赖查找和源码目标生成前，
以 `Windows support is not available in this revision: native *_win.cpp implementations are not yet included.`
错误停止配置。`win-dev` preset 仅在 Windows 宿主可用，不表示当前修订支持 Windows 构建。

## 构建选项与程序资源

`src/CMakeLists.txt` 的 `dagent_compile_options(target)` 为自有目标设置 PRIVATE 编译选项，
用 `COMPILE_LANG_AND_ID` 分别选择 C 和 C++：GNU/Clang 均为 `-Wall -Wextra`；
MSVC 的 C 为 `/W4 /utf-8`，C++ 为 `/W4 /permissive- /utf-8`。
函数不设置编译特性，不改变各目标已有的 `cxx_std_23` 声明，也不修改第三方目标的选项。

SRT bridge/wrapper 与 SearXNG 入口的构建复制目标、可执行文件对它们的依赖及安装规则仅在 Linux 启用。
AppArmor profile 的构建生成和安装生成也仅在 Linux 启用，后者仍使用不含 `DESTDIR` 的最终后端路径。
Home 安装脚本按配置时的目标系统选择 Linux 的 0600 权限设置和 AppArmor 生成；
仅补充缺失用户文件的规则保持共享。Linux 的资源路径、文件权限和 profile 内容不变。

## 边界

- I1：自有 C++ 中的平台头文件和原生平台操作集中到 `src/private/**/*_posix.cpp`。
  范围包括 unistd/fcntl/poll/signal/termios/spawn/pthread、sys/linux 头文件，进程与信号、fd I/O、
  socketpair、文件锁、原生文件和目录操作、终端、时间转换、uname、监听及 `/proc` 路径。
  第三方 `src/public/lib/**`、SRT 资源脚本和 bash 语义分析字面量不受此限制；
  Boost.Asio/Process 的跨平台用法可以留在共享实现，POSIX 钩子与 fd 操作属于平台实现。
- I2：`src/public/**`（除 lib）不直接包含平台头文件，不在接口或成员中暴露 fd、pid_t、
  termios、sigset_t 等原生类型；资源状态用不透明类型表示。标准库类型可用于中立接口。
- 平台提取保留原操作、标志、顺序、错误和日志文本、超时、权限与参数；格式化和业务失败策略留在调用方。

## 平台文件与接口

| 平台文件 | 中立声明 | 实现 |
| --- | --- | --- |
| `src/private/base/platform_posix.cpp` | `src/public/base/platform.hpp`、`src/public/base/path.hpp` | `path_from_utf8`、`path_to_utf8`、`path_to_generic_utf8`、`executable_path`、`environment_variable`、`home_directory`、`process_id`、`stdin_is_terminal`、`stdout_is_terminal`、`local_time`、`utc_time`、`utc_timestamp`、`can_execute`、`default_temporary_directory`、`create_unique_directory` |
| `src/private/workspace/context_posix.cpp` | `src/public/workspace/context.hpp` | `detail::os_description`、`detail::shell_description` |
| `src/private/app/cli_posix.cpp` | `src/public/app/cli.hpp` | `detail::read_stdin_all` |
| `src/private/app/paths_posix.cpp` | `src/public/app/paths.hpp` | `detail::executable_home_directory`、`detail::probe_home_writable` |
| `src/private/app/interrupts_posix.cpp` | `src/public/app/interrupts.hpp` | `install_interrupts`、`ignore_broken_pipe` |
| `src/private/app/backend_main_posix.cpp` | `src/public/app/interrupts.hpp` | `initialize_backend_signals` |
| `src/private/app/toolchain_posix.cpp` | `src/public/app/toolchain_detail.hpp` | `detail::runtime_platform`、`detail::SyncLock` |
| `src/private/ui/browser_posix.cpp` | `src/public/ui/browser.hpp` | `open_browser` |
| `src/private/web/document_posix.cpp` | `src/public/web/detail.hpp` | `detail::convert_charset` |
| `src/private/ipc/channel_posix.cpp` | `src/public/ipc/channel.hpp` | `InheritedEndpoint` 参数解析、`Channel` 原生状态与传输、`BackendProcess` 不透明进程标识、`spawn_backend`、`wait_or_terminate` |
| `src/private/tui/terminal_posix.cpp` | `src/public/tui/terminal.hpp`、`src/public/tui/event_wait.hpp` | Terminal 生命周期、原生状态、尺寸、字节写入、输入恢复与信号；EventWait 的等待、输入读取、唤醒及排空 |
| `src/private/exec/platform_posix.cpp` | `src/public/exec/detail.hpp`、`src/public/exec/platform.hpp`、`src/public/exec/process.hpp` | `inherited_environment`、`environment_names_equal`、`environment_name_matches`、`resolve_program`、`system_git_program`、`default_search_path`、`project_search_path`、执行权限、SIGPIPE、ProcessGroup、退出状态、spawn_command/spawn_child、InheritedResources、SrtControl、默认 SRT 可读目录 |
| `src/private/workspace/files_posix.cpp` | `src/public/workspace/files.hpp` | `probe`、`read_text`、`write_text`、`stamp_of`、FileReference 原生状态与操作 |
| `src/private/storage/storage_posix.cpp` | `src/public/storage/platform.hpp` | `detail::SessionFileLock` |
| `src/private/storage/decision_store_posix.cpp` | `src/public/storage/platform.hpp` | `detail::make_decision_file_private` |
| `src/private/app/config_detail_posix.cpp` | `src/public/app/config_detail.hpp` | `detail::require_private_file` |
| `src/private/app/models_posix.cpp` | `src/public/app/config_detail.hpp` | `detail::ModelWriteLock` |
| `src/private/llm/auth_posix.cpp` | `src/public/llm/auth.hpp`、`src/public/llm/auth_detail.hpp` | `read_credentials`、`detail::CredentialLock`、`detail::write_credentials`、`detail::remove_credentials` |
| `src/private/app/queries_posix.cpp` | `src/public/app/queries.hpp` | `QueryGatewayImpl::watch_workspace` |
| `src/private/app/sandbox_status_posix.cpp` | `src/public/app/sandbox_status.hpp` | `detail::host_bwrap`、`detail::host_socat`、`detail::append_host_diagnostics` |

## 接口语义与资源所有权

`executable_path` 的两个重载分别保留 filesystem 的抛异常和 error_code 行为，供 IPC、sandbox status、
SearXNG 定位程序资源。安装根的固定缓冲区 readlink 和 InstallError 在 `app/paths_posix.cpp` 中保持独立；
`app/paths.cpp` 保留 DAGENT_HOME/dev 默认值选择和目录验证，可写探针仍使用原 O_EXCL、0600 与关闭/删除顺序。

`path_from_utf8` 在 POSIX 按原始字节构造路径，`path_to_utf8` 和 `path_to_generic_utf8` 分别保留
原 `string()`、`generic_string()` 的输出形式；不校验或修复非法 UTF-8，不规范化、不解析符号链接。
workspace 的用户路径、Git 根目录和参数通过这些接口转换；上下文路径仍只在生成 JSON 展示副本时调用
`to_valid_utf8`，文件操作继续使用原始路径。UI 的项目路径缩写前通过 `path_to_utf8` 取文本。

`environment_variable` 返回环境变量的独立 `optional<string>` 副本，不缓存；未设置为 `nullopt`，
已设置为空为有值的空字符串。`home_directory` 只读取 HOME，返回相同契约，不规范化，且不是 DAGENT_HOME。
app 通过环境变量接口读取 DAGENT_HOME，仅非空时使用；dev 默认值与可执行文件目录的选择顺序不变。
tools 的 `~` 展开在 HOME 缺失或为空时保留输入；权限层、命令分类和 UI 缩写只判断 HOME 是否存在，
保留空值参与原路径拼接或文本比较的行为。命令分类的局部 lambda 借用函数内持有的 Home 副本，不逃逸。

`process_id` 返回当前进程号，调用位置与十进制格式不变。时间转换写入调用方的 `std::tm`，
local/UTC 转换返回是否成功，UTC 时间戳转换保留原返回值和对 tm 的修改；调用者维持各自的失败处理。

`can_execute` 只检查执行权限，不附加普通文件判断；exec 的 stat 检查由 `regular_executable` 在平台实现中完成。
`create_unique_directory` 接收 `filesystem::path` 形式的完整 XXXXXX 模板，复制原生字节后调用 mkdtemp，
成功返回目录，失败返回 strerror 文本。`default_temporary_directory` 在 POSIX 固定返回 `/tmp`，
不读取 TMPDIR 等环境变量，供 SRT 与 MCP 的私有 HOME 使用。SRT 控制目录既有的
`filesystem::temp_directory_path()` 回退保持独立；SearXNG 仍在 Home 的 run 下创建目录。
父目录创建、业务异常、资源获取及清理顺序仍由各调用方保留。

`os_description` 返回 uname 的 `sysname release`，失败为 `unknown`；`shell_description` 返回非空 SHELL，
未设置或为空时返回 `sh`，只用于上下文展示。执行 Bash 仍由 Toolchain 的程序记录和 `exec::Environment::shell` 决定。
Toolchain 的 `runtime_platform`
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
`inherited_environment` 在父进程中返回有所有权的字符串快照，保留原环境顺序与重复项，不缓存。
`make_environment` 直接使用快照容器并稳定过滤无名、无等号和 env_deny 匹配项，随后依次应用默认项、
`Options::environment`、`env_unset`、`env_set`。`environment_names_equal` 在 POSIX 区分大小写；
`environment_name_matches` 独立保留 fnmatch/FNM_CASEFOLD。赋值只覆盖第一个同名项，查询取第一个，
unset 删除全部同名项，不额外去重。fork 后钩子不展开不透明对象、不新增分配。

`resolve_program` 接收完整环境，POSIX 只从其中读取 PATH，不回退宿主环境。带目录的程序路径仍相对 cwd
检查；裸程序名按冒号拆分的 PATH 搜索，空分量表示 cwd，整个 PATH 为空或缺失时不搜索裸程序名。
Runner 和 Child 传入合并后的子进程环境；`which` 传入未经过滤和覆盖的宿主环境快照。
`system_git_program` 固定返回 `/usr/bin/git`，不预先检查或增加 fallback，供 context 和 queries 使用；
workspace 的 HEAD diff 仍执行 `git`，通过其传入 PATH 搜索。
`default_search_path` 返回 `/usr/bin:/bin`，只读 SRT 在原位置追加该变量。
`project_search_path` 供 project 受限 Bash 使用：非空宿主 PATH 原样保留，未设置或空值时回退到
`/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`。project 宿主 Bash 和 HEAD diff 则通过
`base::environment_variable` 读取 PATH，缺失时不追加，空值时追加空 PATH，不使用六目录回退。

SRT 配置 JSON、控制帧和线程编排保持共享，各控制 I/O 方法保留原短写、EINTR、EPIPE 与关闭语义。

workspace 的路径和文本编解码通过 `files_detail.hpp` 供平台文件调用，不暴露原生句柄或 stat。
两条写入路径保持独立：通用 write_text 保留 `07777`、显式 close 后 rename；FileReference::write 保留 `0777`，
文件句柄保持打开直到 renameat2、同步目录和读取返回 Stamp 后清理。两者共用原 umask 静态缓存，
零字节写处理与异常清理顺序见 [workspace](workspace.md#2-文件原语fileshpp)。

SessionFileLock 的 Impl 在保存会话 ID 后、open 锁文件前分配；同进程租约注册表与互斥范围仍在 storage.cpp。
锁保持非阻塞、无 EINTR 重试，最后共享 State 销毁时 UNLOCK+close。决策库 chmod(0600) 仍忽略返回值，
不与其他模块的锁或权限策略合并，详见 [storage](storage.md#4-写所有权)。

配置的 stat 检查单独保留原规则；ModelWriteLock 用 pimpl 保存 fd，阻塞加锁仅 EINTR 重试，析构 UNLOCK 后 close。
CredentialLock 同样用 pimpl，但保留非阻塞 flock 的 30 秒期限与 50ms 可取消等待，析构只关闭锁文件和目录。
两种 Impl 都在原目录操作前分配，不合并锁策略。公共头不再暴露 ModelWriteLock 的 fd。
凭据 JSON 编解码在共享 auth.cpp，读写的平台函数在原 try 位置调用，转换期间保持原 fd 持有期；
刷新仍在同一锁范围内重读、记录 refresh_pending、发送 HTTP 和保存新凭据，详见 [llm](llm.md#7-登录提供方与凭据阶段-3a)。

QueryGatewayImpl 持有 watcher jthread，重新设置监听先停止并等待旧线程；空回调只停止监听。
线程内部持有 inotify/eventfd，取消回调写 eventfd 唤醒 poll；取消回调销毁后才按原顺序关闭两个 fd。
目录保护判断由共享私有成员 `should_watch_directory` 提供，原生实现保留根目录加入、4096 上限、事件掩码、重扫与通知顺序。

宿主 bwrap/socat 查找先走 exec::which，再按原规则检查固定后备路径；返回路径值，不持有进程或文件资源。
AppArmor 诊断保持在真实 sandbox probe 之后追加，缺少 runtime 的提前返回不读取宿主诊断。
资源脚本的 Unix socket、信号和文件操作仍在 SRT bridge/wrapper 内；这些脚本与第三方代码属于 I1 的明确例外。
Git 命令参数及 unified diff 中的 `/dev/null` 是外部命令配置或文本格式，bash 分析中的设备路径是待分析命令的语义，
均不是共享 C++ 对本机原生 API 的调用。中立封装、标准流或跨平台库的 read/write/close 等同名方法也不等同于 fd 系统调用。
