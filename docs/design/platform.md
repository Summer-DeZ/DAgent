# 平台实现约定

平台源文件由根 CMake 根据 `CMAKE_SYSTEM_NAME` 设置的 `DAGENT_PLATFORM` 选择：Linux 为 `posix`，
Windows 为 `win`，其他系统以 `Unsupported target system` 错误拒绝配置。`src/CMakeLists.txt`
显式列出 `private/<模块>/<文件>_${DAGENT_PLATFORM}.cpp`，不使用运行时平台选择或虚函数端口。
当前包含完整 POSIX 实现及 base、workspace/context、app/cli、app/paths、ui/browser、web/document、
exec/platform、ipc/channel、app/interrupts、app/backend_main、workspace/files 十一个 Windows 实现。
Windows 在 `project()` 完成工具链与编译器识别后、项目依赖查找和源码目标生成前，
以 `Windows support is not available in this revision: native *_win.cpp implementations are not yet complete.`
错误停止配置。`win-dev` preset 仅在 Windows 宿主可用，不表示当前修订支持 Windows 构建。

## 构建选项与程序资源

`src/CMakeLists.txt` 的 `dagent_compile_options(target)` 为自有目标设置 PRIVATE 编译选项，
用 `COMPILE_LANG_AND_ID` 分别选择 C 和 C++：GNU/Clang 均为 `-Wall -Wextra`；
MSVC 的 C 为 `/W4 /utf-8`，C++ 为 `/W4 /permissive- /utf-8`。
函数不设置编译特性，不改变各目标已有的 `cxx_std_23` 声明，也不修改第三方目标的选项。
该函数还通过 `PLATFORM_ID:Windows` 为自有目标添加 PRIVATE 的 `NOMINMAX`、`WIN32_LEAN_AND_MEAN`、
`_WIN32_WINNT=0x0A00` 定义；Linux 不增加定义或编译选项。
Windows 的 CMake CRT 选择为 `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`，与 vcpkg x64-windows 的动态 CRT 一致。
依赖规则在 Windows 查找 Boost 的 process 组件和 `find_package(Iconv MODULE REQUIRED)`；
exec 保留 `BOOST_PROCESS_USE_STD_FS`，使用具体的 Boost.Process v2 分头文件，并显式链接 `Boost::process`、`ws2_32` 和 `bcrypt`。
Windows 系统库按使用目标链接：base 用 `bcrypt`，workspace 用 `ntdll`、`advapi32`、`bcrypt`，ui 和 app_cli 用 `shell32`；
ipc 的私有管道 ACL 和随机名称使用 `advapi32`、`bcrypt`；web 用 `Iconv::Iconv`。这些依赖均不会进入 Linux 链接行；Windows 的提前停止点仍先于依赖查找。

SRT bridge/wrapper 与 SearXNG 入口的构建复制目标、可执行文件对它们的依赖及安装规则仅在 Linux 启用。
AppArmor profile 的构建生成和安装生成也仅在 Linux 启用，后者仍使用不含 `DESTDIR` 的最终后端路径。
Home 安装脚本按配置时的目标系统选择 Linux 的 0600 权限设置和 AppArmor 生成；
仅补充缺失用户文件的规则保持共享。Linux 的资源路径、文件权限和 profile 内容不变。

## 边界

- I1：自有 C++ 中的平台头文件和原生平台操作集中到 `src/private/**/*_posix.cpp` 或 `*_win.cpp`。
  范围包括 unistd/fcntl/poll/signal/termios/spawn/pthread、sys/linux 头文件，进程与信号、fd I/O、
  socketpair、文件锁、原生文件和目录操作、终端、时间转换、uname、监听及 `/proc` 路径。
  第三方 `src/public/lib/**`、SRT 资源脚本和 bash 语义分析字面量不受此限制；
  Boost.Asio/Process 的跨平台用法可以留在共享实现，POSIX 钩子与 fd 操作属于平台实现。
  iconv 是两个平台共用的库接口，转换循环位于共享 `web/charset.cpp`。
- I2：`src/public/**`（除 lib）不直接包含平台头文件，不在接口或成员中暴露 fd、pid_t、
  termios、sigset_t 等原生类型；资源状态用不透明类型表示。标准库类型可用于中立接口。
- 平台提取保留原操作、标志、顺序、错误和日志文本、超时、权限与参数；格式化和业务失败策略留在调用方。

## 平台文件与接口

| 平台文件 | 中立声明 | 实现 |
| --- | --- | --- |
| `src/private/base/platform_{posix,win}.cpp` | `src/public/base/platform.hpp`、`src/public/base/path.hpp` | `path_from_utf8`、`path_to_utf8`、`path_to_generic_utf8`、`executable_path`、`environment_variable`、`home_directory`、`process_id`、`stdin_is_terminal`、`stdout_is_terminal`、`local_time`、`utc_time`、`utc_timestamp`、`can_execute`、`default_temporary_directory`、`create_unique_directory` |
| `src/private/workspace/context_{posix,win}.cpp` | `src/public/workspace/context.hpp` | `detail::os_description`、`detail::shell_description` |
| `src/private/app/cli_{posix,win}.cpp` | `src/public/app/cli.hpp` | `detail::read_stdin_all` |
| `src/private/app/paths_{posix,win}.cpp` | `src/public/app/paths.hpp` | `detail::executable_home_directory`、`detail::probe_home_writable` |
| `src/private/app/interrupts_{posix,win}.cpp` | `src/public/app/interrupts.hpp` | `install_interrupts`、`ignore_broken_pipe` |
| `src/private/app/backend_main_{posix,win}.cpp` | `src/public/app/interrupts.hpp` | `initialize_backend_signals` |
| `src/private/app/toolchain_posix.cpp` | `src/public/app/toolchain_detail.hpp` | `detail::runtime_platform`、`detail::extract_runtime_archive`、`detail::runtime_environment`、`detail::install_python_runtime`、`detail::python_environment_program`、`detail::SyncLock` |
| `src/private/ui/browser_{posix,win}.cpp` | `src/public/ui/browser.hpp` | `open_browser` |
| `src/private/web/document_{posix,win}.cpp` | `src/public/web/detail.hpp` | `detail::convert_charset`（共用 `web/charset.cpp` 的 `convert_charset_iconv`） |
| `src/private/ipc/channel_{posix,win}.cpp` | `src/public/ipc/channel.hpp` | `InheritedEndpoint` 参数解析、`Channel` 原生状态与传输、`BackendProcess` 不透明进程标识、`backend_binary_path`、`spawn_backend`、`wait_or_terminate` |
| `src/private/tui/terminal_posix.cpp` | `src/public/tui/terminal.hpp`、`src/public/tui/event_wait.hpp` | Terminal 生命周期、原生状态、尺寸、字节写入、输入恢复与信号；EventWait 的等待、输入读取、唤醒及排空 |
| `src/private/exec/platform_{posix,win}.cpp` | `src/public/exec/detail.hpp`、`src/public/exec/platform.hpp`、`src/public/exec/process.hpp` | `inherited_environment`、`environment_names_equal`、`environment_name_matches`、`resolve_program`、`system_git_program`、`default_search_path`、`project_search_path`、执行权限、SIGPIPE、ProcessGroup、退出状态、spawn_command/spawn_child、InheritedResources、SrtControl、默认 SRT 可读目录 |
| `src/private/workspace/files_{posix,win}.cpp` | `src/public/workspace/files.hpp` | `probe`、`read_text`、`write_text`、`stamp_of`、FileReference 原生状态与操作、DirectoryEntries 惰性枚举 |
| `src/private/storage/storage_posix.cpp` | `src/public/storage/platform.hpp` | `detail::SessionFileLock` |
| `src/private/storage/decision_store_posix.cpp` | `src/public/storage/platform.hpp` | `detail::make_decision_file_private` |
| `src/private/app/config_detail_posix.cpp` | `src/public/app/config_detail.hpp` | `detail::require_private_file` |
| `src/private/app/models_posix.cpp` | `src/public/app/config_detail.hpp` | `detail::ModelWriteLock` |
| `src/private/llm/auth_posix.cpp` | `src/public/llm/auth.hpp`、`src/public/llm/auth_detail.hpp` | `read_credentials`、`detail::CredentialLock`、`detail::write_credentials`、`detail::remove_credentials` |
| `src/private/app/queries_posix.cpp` | `src/public/app/queries.hpp` | `QueryGatewayImpl::watch_workspace` |
| `src/private/app/sandbox_status_posix.cpp` | `src/public/app/sandbox_status.hpp` | `detail::host_bwrap`、`detail::host_socat`、`detail::append_host_diagnostics` |

## 接口语义与资源所有权

以下未特别说明的原生操作契约描述 POSIX 实现；已有 Windows 实现的差异如下：

- base：路径、环境变量均以 UTF-8 对外，以 UTF-16 调用原生 API；非法编码抛 `system_error`，不修复、
  不规范化路径。可执行文件路径使用可增长缓冲区的 `GetModuleFileNameW`，error_code 重载在成功时清除错误。
  `GetEnvironmentVariableW` 保留缺失与空值区别；用户 Home 只读取 `USERPROFILE`，不改变 DAGENT_HOME 的选择顺序。
  `GetConsoleMode` 判断标准输入输出是否控制台；时间使用 `localtime_s`、`gmtime_s`、`_mkgmtime64`。
  `can_execute` 仅判断存在且非目录，是否为可执行程序由实际启动判断，不模拟 POSIX 执行位。
  `GetTempPath2W` 选择系统临时目录；完整 XXXXXX 模板末尾由 BCrypt 随机后缀替换，`CreateDirectoryW` 原子独占创建，
  仅在重名时重试。目录继承父目录 ACL，不等同于 POSIX 的 0700；函数不创建父目录、不清理返回的目录。
- workspace/context：`RtlGetVersion` 从 ntdll 取得实际系统版本，展示为 `Windows major.minor.build`，失败为 `unknown`；
  shell 展示为 `Git Bash`，不读取 PowerShell 或 SHELL，不负责定位执行程序。
- app/cli：`ReadFile` 原样读取 stdin 字节至 EOF 或错误，不做 CRLF 或代码页转换。共享 `parse_args` 在自定义 argv 扫描前
  调用 `CLI::App::ensure_utf8`；转码后 argv 的所有权属于该 App，Linux 返回原 argv。
- app/paths：通过 base 的可执行文件路径取父目录；`CREATE_NEW` 创建可写探针，成功后依次关闭和删除，
  不覆盖已有同名文件。Home 选择与业务错误上下文仍由共享层保留，原生错误来自 Windows。
- ui/browser：URL 从 UTF-8 转为 UTF-16 后交给 `ShellExecuteW` 的 `open` 动作，不拼 shell 命令；
  返回值只表示启动请求是否被系统接受，不等待浏览器退出。
- web/document：Windows 链接 libiconv，POSIX 使用 glibc iconv，共用同一转换循环及错误文本。
  该解码实现不改变工具层对只读 SRT 的要求。

Windows 的 exec 与进程边界：

- `GetEnvironmentStringsW` 返回原环境条目的有所有权 UTF-8 快照。变量名经 UTF-16 转换，以
  `CompareStringOrdinal(..., TRUE)` 比较，因此 `environment_names_equal` 不再承诺 noexcept；
  共享查询按实际等号位置截取值，不以查询名的 UTF-8 字节数推断位置。合并阶段仍稳定过滤、不去重，
  仅在生成 Windows 环境块时按名称稳定排序，保留同名项相对顺序；块以双 NUL 结尾，启动使用 `CREATE_UNICODE_ENVIRONMENT`。
  deny 匹配支持 `*`、`?`、括号集合、否定集合、范围和反斜杠转义，按 UTF-16 字符进行 ordinal 忽略大小写比较；
  不支持 POSIX 命名字符类。POSIX 保留原 fnmatch/FNM_CASEFOLD。
- 程序解析只读取传入环境中的 PATH/PATHEXT，PATH 按分号拆分，空分量表示 cwd，可去掉目录分量外层双引号；
  整个 PATH 缺失或为空时不搜索裸程序名，不隐式搜索宿主当前目录、系统目录或宿主 PATH。
  显式 `.exe`/`.com` 可直接检查；无扩展名时只按 PATHEXT 中的 `.exe`/`.com` 顺序补全，
  PATHEXT 缺失或为空时默认 `.EXE;.COM`。拒绝 `.cmd`/`.bat` 等其他扩展名及依赖每驱动器 cwd 的 `C:foo` 路径。
  `regular_executable` 检查扩展名、存在且非目录，最终可执行性由 CreateProcess 决定。
- 系统 Git 固定为 `C:\Program Files\Git\cmd\git.exe`；默认 PATH 顺序为同安装的 cmd、bin、Windows 系统目录、Windows 目录。
  `project_search_path` 保留非空宿主 PATH，否则使用这个默认值。执行 Bash 的选择仍由 Toolchain 和 Environment 负责。
- Boost.Process v2 Windows launcher 使用 overlapped stdio 管道、UTF-16 argv 和环境块、显式 `inherited_handles`。
  无 stdin 数据时使用 NUL；merge 模式复用 stdout 子端；长期 Child 固定三条管道。
  每次启动挂起并新建控制台进程组；成功钩子移交主线程句柄，`ProcessGroup::capture` 加入独立 Job 后才 ResumeThread。
  加入或恢复失败时终止并等待该进程，报告启动失败。Job 设置 KILL_ON_JOB_CLOSE，销毁也会清理残留后代。
  `terminate` 尝试发 Ctrl-Break；没有共同控制台或程序不处理时，仍由既有宽限计时器执行 `TerminateJobObject`。
  退出状态的 signal 为空，DWORD 退出码保留 32 位到既有 int 接口（最高位为 1 时显示为负数），不作 POSIX 状态解码。
- InheritedResources 保存不拥有资源的句柄清单；launch 临时设置并恢复额外句柄的继承位，仅白名单进入子进程。
  SIGPIPE 操作为空；SrtControl 构造、通信入口及默认 SRT 可读路径明确抛 `sandbox is unsupported on Windows`，
  无资源的 close/shutdown/析构为空操作，不建立伪造连接，不改变授权或 MCP 的上层启动策略。

Windows 的 IPC 与中断边界：

- `Channel::create_pair` 创建 128 位随机名、仅当前用户 DACL、拒绝远程访问的双向字节管道，
  父进程预先打开客户端并完成连接。两个方向各自有 OVERLAPPED 和手动复位事件；
  短写仍交给共享分帧循环，底层同步方法等待各自 I/O 完成。
- shutdown 与提交 I/O 使用同一把锁：先阻止新操作，再 CancelIoEx，等待在途读写结束后关闭本端管道，通知对端 EOF。
  Windows 不保留 socket 式的半关闭句柄；close 幂等，不清空帧缓冲、错误和 EOF 状态。
  `--ipc-fd` 名称不变，值严格解析为十进制 uintptr_t，不接受溢出、尾随文本、0 或 INVALID_HANDLE_VALUE。
  解析对象不拥有管道；adopt 接管后由 Channel 关闭，并清除继承位。
- `backend_binary_path` 在两个平台文件内实现；Windows 在前端可执行文件同目录找 `dagent-backend.exe`。
  CreateProcessW 使用 STARTUPINFOEX，显式继承三份 stdio 副本和子端 IPC 句柄，父进程立即关闭子端副本。
  后端挂起创建并加入 KILL_ON_JOB_CLOSE 生命周期 Job 后恢复，失败即终止；后端及其工具的嵌套 Job 均留在生命周期 Job 中。
  BackendProcess 的共享状态拥有进程和 Job 句柄，最后一个状态销毁关闭 Job，前端异常退出同样触发系统清理。
  `wait_or_terminate` 先断开连接并等待宽限期，超时 TerminateJobObject 后等待后端退出；强制终止返回 false。
- 前端 SetConsoleCtrlHandler 只处理 Ctrl-C/Ctrl-Break：首次在 graceful 为真时计数并通知事件，工作线程执行 request_stop；
  轮外或第二次中断直接 TerminateProcess(130)，不等待第一次 stop 回调结束。状态和事件持续到进程结束。
  后端忽略这两种控制台事件，取消由前端经 IPC 传入；ignore_broken_pipe 为空操作。

SQLite 的两个打开入口通过 `path_to_utf8` 向 `sqlite3_open_v2` 传入文件名；POSIX 保留路径原始字节，
Windows 从原生 UTF-16 转为 SQLite 要求的 UTF-8，临时字符串覆盖整个调用期间。

`executable_path` 的两个重载分别保留 filesystem 的抛异常和 error_code 行为，供 IPC、sandbox status、
SearXNG 定位程序资源。安装根的固定缓冲区 readlink 和 InstallError 在 `app/paths_posix.cpp` 中保持独立；
`app/paths.cpp` 保留 DAGENT_HOME/dev 默认值选择和目录验证，可写探针仍使用原 O_EXCL、0600 与关闭/删除顺序。

`path_from_utf8` 在 POSIX 按原始字节构造路径，`path_to_utf8` 和 `path_to_generic_utf8` 分别保留
原 `string()`、`generic_string()` 的输出形式；不校验或修复非法 UTF-8，不规范化、不解析符号链接。
workspace 的用户路径、Git 根目录和参数通过这些接口转换；上下文路径仍只在生成 JSON 展示副本时调用
`to_valid_utf8`，文件操作继续使用原始路径。UI 的项目路径缩写前通过 `path_to_utf8` 取文本。

`environment_variable` 返回环境变量的独立 `optional<string>` 副本，不缓存；未设置为 `nullopt`，
已设置为空为有值的空字符串。POSIX 的 `home_directory` 只读取 HOME，返回相同契约，不规范化，且不是 DAGENT_HOME。
app 通过环境变量接口读取 DAGENT_HOME，仅非空时使用；dev 默认值与可执行文件目录的选择顺序不变。
tools 的 `~` 展开在 HOME 缺失或为空时保留输入；权限层、命令分类和 UI 缩写只判断 HOME 是否存在，
保留空值参与原路径拼接或文本比较的行为。命令分类的局部 lambda 借用函数内持有的 Home 副本，不逃逸。
配置的 `env_value` 也使用环境变量副本，缺失与空值都沿用原来的 unset 错误；DAGENT_LOG 仅在非空时解析。
SearXNG 代理透传只判断变量是否存在，保留空值及大小写变量的原追加顺序。
Terminal 的 `probe_caps() noexcept` 保留不分配内存的 getenv/string_view 读取，TERM/COLORTERM 的缺失与空值
均作为空文本参与原能力判断；此处不调用返回字符串副本的接口。

`process_id` 返回当前进程号，调用位置与十进制格式不变。时间转换写入调用方的 `std::tm`，
local/UTC 转换返回是否成功，UTC 时间戳转换保留原返回值和对 tm 的修改；调用者维持各自的失败处理。

POSIX 的 `can_execute` 只检查执行权限，不附加普通文件判断；exec 的 stat 检查由 `regular_executable` 在平台实现中完成。
POSIX 的 `create_unique_directory` 接收 `filesystem::path` 形式的完整 XXXXXX 模板，复制原生字节后调用 mkdtemp，
成功返回目录，失败返回 strerror 文本。`default_temporary_directory` 在 POSIX 固定返回 `/tmp`，
不读取 TMPDIR 等环境变量，供 SRT 与 MCP 的私有 HOME 使用。SRT 控制目录既有的
`filesystem::temp_directory_path()` 回退保持独立；SearXNG 仍在 Home 的 run 下创建目录。
父目录创建、业务异常、资源获取及清理顺序仍由各调用方保留。

`os_description` 返回 uname 的 `sysname release`，失败为 `unknown`；`shell_description` 返回非空 SHELL，
未设置或为空时返回 `sh`，只用于上下文展示。执行 Bash 仍由 Toolchain 的程序记录和 `exec::Environment::shell` 决定。
Toolchain 的 `runtime_platform`
保留 `sysname-machine` 与原异常。`SyncLock` 用 pimpl 保存 fd，仍非阻塞加独占锁，析构只 close，
锁范围仍覆盖原 sync 流程；文件锁未合并为 base 原语。
Toolchain 的解包、执行环境构造、uv Python 安装与符号链接、Python 环境程序位置也由该平台文件提供。
POSIX 保留固定 tar 路径和参数、PATH 拼接和环境顺序、uv 参数与链接目标；
`execute_runtime_command` 声明在同一 detail 头，实现在共享 `toolchain.cpp`，仍负责 20 分钟超时、
65536 输出上限、原失败文本和尾部 CR/LF 清理。下载、校验、摘要、代际及发布留在共享层，详见 [toolchain](toolchain.md)。

CLI 用 `stdin_is_terminal` 判断输入来源，用 `read_stdin_all` 读取管道输入，保留 EINTR 重试与其他错误时停止读取。
Terminal 也通过 base 判断 stdin/stdout 是否终端。前端先安装中断处理，再忽略断管信号；后端参数校验后恢复信号屏蔽并忽略断管信号，
随后接管 Channel。后端 `--ipc-fd` 保留原参数格式，参数值经不透明的 `InheritedEndpoint` 解析和接管。

POSIX 浏览器启动完整实现在 `browser_posix.cpp`，保留原 posix_spawn 属性与回收线程。
网页 charset/BOM 检测留在 `document.cpp`，平台文件转调共享 `charset.cpp` 的 iconv 转换循环，保留缓冲区、截断与错误处理。

IPC 的共享文件只保留分帧、缓冲与错误状态，后端路径位于平台文件。Channel 和继承参数对象共用不透明端点状态，
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

Windows 文件原语使用 NtCreateFile 的 RootDirectory 逐分量打开，prepare 仅获取元数据权限，
固定最近存在的祖先及目标；拒绝 reparse point、上跳与 ADS。validate 以 FILE_ID_INFO 的卷序列及完整 128 位标识
确认祖先与目标。Stamp 新增默认零的 inode_high，参与默认相等比较；POSIX 仍为零，没有新增 Stamp 序列化格式。

FileReference 的 directory_path 被 directory_entries 取代；DirectoryEntries 只暴露名称、惰性类型查询及
error_code/抛异常的递增接口，独立持有固定目录资源。POSIX 内部复制原 fd 并沿 /proc 路径使用既有标准库迭代器；
Windows 在 ReOpenFile 获得的目录句柄上分批枚举，仅类型查询才相对打开子项且不跟随 reparse point。
read 先权限过滤再查类型，memory 只消费名称；共享调用方不再持有平台目录路径。

Windows 写入在固定目录下创建临时文件并 FlushFileBuffers，保留已有目标的 owner/group/DACL 与继承保护状态，
不读取/复制特权 SACL，不在权限失败时放宽 ACL。普通写入和 FileReference 写入分别保持各自的编码、校验和覆盖策略。
FILE_RENAME_INFO 通过 FileRenameInfoEx 相对固定父目录提交；替换允许旧目标句柄继续存在，新建禁止覆盖。
FileReference 提交前也比较新建/打开的最终父目录标识；提交后关闭数据写句柄，再从固定的新对象读取最终 Stamp。
删除用 FileDispositionInfoEx；失败临时文件按持有句柄清理。Windows 的共享访问、只读属性及文件系统 API 支持可能导致
替换/删除失败；目录 flush 为尽力而为。完整差异见 [workspace](workspace.md#windows-文件原语)。

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
