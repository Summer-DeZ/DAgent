# 平台实现约定

平台源文件由根 CMake 根据 `CMAKE_SYSTEM_NAME` 设置的 `DAGENT_PLATFORM` 选择：Linux 为 `posix`，
Windows 为 `win`，其他系统以 `Unsupported target system` 错误拒绝配置。`src/CMakeLists.txt`
显式列出 `private/<模块>/<文件>_${DAGENT_PLATFORM}.cpp`，不使用运行时平台选择或虚函数端口。
当前包含完整 POSIX 实现及 base、workspace/context、app/cli、app/paths、ui/browser、web/document、
exec/platform、ipc/channel、app/interrupts、app/backend_main、workspace/files、storage/storage、storage/decision_store、
app/config_detail、app/models、llm/auth、app/toolchain、tui/terminal、app/queries、app/sandbox_status 二十个 Windows 实现。
Windows 配置可用：在 MSVC 开发环境中使用 Windows 宿主专属的 `win-dev` 或 `win-release` preset，并按 vcpkg 实际安装位置传入
`-DCMAKE_TOOLCHAIN_FILE`；配置随后照常查找依赖并生成全部目标。Windows 终端设计见
[Windows：终端](#windows终端)。

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
Windows 系统库按使用目标链接：base 用 `bcrypt`、`advapi32`，workspace 用 `ntdll`、`advapi32`、`bcrypt`，ui 和 app_cli 用 `shell32`；
ipc 的私有管道 ACL 和随机名称使用 `advapi32`、`bcrypt`；llm 用 `ntdll`、`advapi32`、`bcrypt`，app_config 用 `advapi32`；web 用 `Iconv::Iconv`。这些依赖均不会进入 Linux 链接行。

SearXNG 入口的构建复制目标、可执行文件对它的依赖及安装规则仅在 Linux 启用；构建目录的 `src/libexec/` 只有
`searxng_server.py`，安装后的 `libexec/` 另有按最终路径生成的 `dagent.apparmor`。MXC 原生库不由 CMake 构建或安装，
而由 `runtime sync` 准备。
AppArmor profile 的构建生成和安装生成也仅在 Linux 启用，后者仍使用不含 `DESTDIR` 的最终后端路径。
Windows 的安装目标登记 `RUNTIME_DEPENDENCY_SET`，从 vcpkg 的运行时目录收集依赖 DLL 并安装到可执行文件旁；
排除 `api-ms-win-*`、`ext-ms-*` 等转发 DLL 以及 System32/WinSxS 中的系统组件，不收集 VC 运行库。
非 vcpkg 构建不添加依赖搜索目录。Windows 不安装 `libexec`，也不生成 AppArmor profile。
Home 安装脚本按配置时的目标系统选择 Linux 的 0600 权限设置或 Windows 的私有 DACL 设置，以及 Linux 的 AppArmor 生成；
Windows 下只对本次新生成的 `config/models.json`、`config/mcp.json` 用 `icacls` 设置受保护 DACL，
仅授予当前用户、SYSTEM 与 Administrators 完全控制。仅补充缺失用户文件的规则保持共享。
Linux 的资源路径、文件权限和 profile 内容不变。

## 边界

- I1：自有 C++ 中的平台头文件和原生平台操作集中到 `src/private/**/*_posix.cpp` 或 `*_win.cpp`。
  范围包括 unistd/fcntl/poll/signal/termios/spawn/pthread、sys/linux 头文件，进程与信号、fd I/O、
  socketpair、文件锁、原生文件和目录操作、终端、时间转换、uname、监听及 `/proc` 路径。
  第三方 `src/public/lib/**` 和 bash 语义分析字面量不受此限制；
  Boost.Asio/Process 的跨平台用法可以留在共享实现，POSIX 钩子与 fd 操作属于平台实现。
  iconv 是两个平台共用的库接口，转换循环位于共享 `web/charset.cpp`。
- I2：`src/public/**`（除 lib）不直接包含平台头文件，不在接口或成员中暴露 fd、pid_t、
  termios、sigset_t 等原生类型；资源状态用不透明类型表示。标准库类型可用于中立接口。
- 平台提取保留原操作、标志、顺序、错误和日志文本、超时、权限与参数；格式化和业务失败策略留在调用方。

## 平台文件与接口

| 平台文件 | 中立声明 | 实现 |
| --- | --- | --- |
| `src/private/base/platform_{posix,win}.cpp` | `src/public/base/platform.hpp`、`src/public/base/path.hpp` | `path_from_utf8`、`path_to_utf8`、`path_to_generic_utf8`、`executable_path`、`environment_variable`、`home_directory`、`process_id`、`stdin_is_terminal`、`stdout_is_terminal`、`local_time`、`utc_time`、`utc_timestamp`、`can_execute`、`make_file_private`、`default_temporary_directory`、`create_unique_directory` |
| `src/private/workspace/context_{posix,win}.cpp` | `src/public/workspace/context.hpp` | `detail::os_description`、`detail::shell_description` |
| `src/private/app/cli_{posix,win}.cpp` | `src/public/app/cli.hpp` | `detail::read_stdin_all` |
| `src/private/app/paths_{posix,win}.cpp` | `src/public/app/paths.hpp` | `detail::executable_home_directory`、`detail::probe_home_writable` |
| `src/private/app/interrupts_{posix,win}.cpp` | `src/public/app/interrupts.hpp` | `install_interrupts`、`ignore_broken_pipe` |
| `src/private/app/backend_main_{posix,win}.cpp` | `src/public/app/interrupts.hpp` | `initialize_backend_signals` |
| `src/private/app/toolchain_{posix,win}.cpp` | `src/public/app/toolchain_detail.hpp` | `detail::runtime_platform`、`detail::extract_runtime_archive`、`detail::runtime_environment`、`detail::install_python_runtime`、`detail::select_python_runtime_program`、`detail::python_environment_program`、`detail::SyncLock` |
| `src/private/ui/browser_{posix,win}.cpp` | `src/public/ui/browser.hpp` | `open_browser` |
| `src/private/web/document_{posix,win}.cpp` | `src/public/web/detail.hpp` | `detail::convert_charset`（共用 `web/charset.cpp` 的 `convert_charset_iconv`） |
| `src/private/ipc/channel_{posix,win}.cpp` | `src/public/ipc/channel.hpp` | `InheritedEndpoint` 参数解析、`Channel` 原生状态与传输、`BackendProcess` 不透明进程标识、`backend_binary_path`、`spawn_backend`、`wait_or_terminate` |
| `src/private/tui/terminal/terminal_posix.cpp` | `src/public/tui/terminal/terminal.hpp`、`src/public/tui/terminal/event_wait.hpp` | Terminal 生命周期、原生状态、尺寸、字节写入、输入恢复与信号；EventWait 的等待、输入读取、唤醒及排空 |
| `src/private/tui/terminal/terminal_win.cpp` | `src/public/tui/terminal/terminal.hpp`、`src/public/tui/terminal/event_wait.hpp` | Windows 控制台版：Terminal 生命周期、能力基线、尺寸、字节写入、输入模式与输出代码页还原、控制台事件；EventWait 的等待、`ReadConsoleInputW` 解码、唤醒及排空 |
| `src/private/exec/platform_{posix,win}.cpp` | `src/public/exec/detail.hpp`、`src/public/exec/platform.hpp`、`src/public/exec/process.hpp` | `inherited_environment`、`environment_names_equal`、`environment_name_matches`、`resolve_program`、`system_git_program`、`default_search_path`、`project_search_path`、执行权限、SIGPIPE、ProcessGroup、退出状态、spawn_command/spawn_child、InheritedResources、`SharedLibrary`、`sandbox_command_line`、`loopback_peer_is_current_user` |
| `src/private/workspace/files_{posix,win}.cpp` | `src/public/workspace/files.hpp` | `probe`、`read_text`、`write_text`、`stamp_of`、FileReference 原生状态与操作、DirectoryEntries 惰性枚举 |
| `src/private/storage/storage_{posix,win}.cpp` | `src/public/storage/platform.hpp` | `detail::SessionFileLock` |
| `src/private/storage/decision_store_{posix,win}.cpp` | `src/public/storage/platform.hpp` | `detail::make_decision_file_private` |
| `src/private/app/config_detail_{posix,win}.cpp` | `src/public/app/config_detail.hpp` | `detail::require_private_file` |
| `src/private/app/models_{posix,win}.cpp` | `src/public/app/config_detail.hpp` | `detail::ModelWriteLock` |
| `src/private/llm/auth_{posix,win}.cpp` | `src/public/llm/auth.hpp`、`src/public/llm/auth_detail.hpp` | `read_credentials`、`detail::CredentialLock`、`detail::write_credentials`、`detail::remove_credentials` |
| `src/private/app/queries_{posix,win}.cpp` | `src/public/app/queries.hpp` | `QueryGatewayImpl::watch_workspace` |
| `src/private/app/sandbox_status_{posix,win}.cpp` | `src/public/app/sandbox_status.hpp` | `detail::platform_sandbox_supported`、`detail::sandbox_containment`、`detail::host_prerequisites`、`detail::append_host_diagnostics` |

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
  该解码实现不改变工具层对只读 MXC 沙箱的要求。

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
  SIGPIPE 操作为空。`SharedLibrary` 用 `LoadLibraryW` 打开、`GetProcAddress` 取符号，失败时 `ExecError{sandbox}` 带系统错误文本；
  `sandbox_command_line` 按 MSYS2 运行库的解析规则拼出 `"<shell>" --noprofile --norc -c "<script>"`（双引号内每个 `\` 和 `"`
  前加 `\`，与 CommandLineToArgvW 不同：MSYS2 会把不在引号前的成对反斜杠也减半）；`sandbox_environment(home)` 返回
  `SystemRoot`、用户真实的 `LOCALAPPDATA` 以及指向私有 HOME 的 `USERPROFILE`、`APPDATA`（POSIX 返回空）；
  `loopback_peer_is_current_user` 第一版固定返回 true。
  Windows 后端为 ProcessContainer：装配加载 `mxc_ffi.dll` 后用代理形状的请求做层级探测，只接受 `base-container`。
  受限执行目前被上游阻塞：Git for Windows 的 bash（MSYS2 运行库）在任何 ProcessContainer 内都无法初始化
  （`msys-2.0.dll` 在全局 `\BaseNamedObjects` 下建目录被拒，microsoft/mxc#1061），真实探针在 filesystem 阶段失败，
  装配按失败关闭回落到无沙箱模式，差异见 [agent 的启动自检未找到可用沙箱时](agent.md#启动自检未找到可用沙箱时)。

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
POSIX 的 `Terminal::probe_caps() noexcept` 保留不分配内存的 getenv/string_view 读取，TERM/COLORTERM 的缺失与空值
均作为空文本参与原能力判断；此处不调用返回字符串副本的接口。

`process_id` 返回当前进程号，调用位置与十进制格式不变。时间转换写入调用方的 `std::tm`，
local/UTC 转换返回是否成功，UTC 时间戳转换保留原返回值和对 tm 的修改；调用者维持各自的失败处理。

POSIX 的 `can_execute` 只检查执行权限，不附加普通文件判断；exec 的 stat 检查由 `regular_executable` 在平台实现中完成。
POSIX 的 `create_unique_directory` 接收 `filesystem::path` 形式的完整 XXXXXX 模板，复制原生字节后调用 mkdtemp，
成功返回目录，失败返回 strerror 文本。`default_temporary_directory` 在 POSIX 固定返回 `/tmp`，
不读取 TMPDIR 等环境变量，供 MXC 受限执行（含 MCP stdio）的私有 HOME 使用；SearXNG 仍在 Home 的 run 下创建目录。
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

Terminal 的 Native 与 EventWait 的 Impl 只在对应平台文件（`terminal_posix.cpp` / `terminal_win.cpp`）定义，
公共头不包含终端类型或原生句柄。
EventWait 只报告就绪状态，Runtime 仍按信号、wake、stdin 的顺序消费，再执行 inbox、尺寸、定时器、Esc、握手和渲染。
`wake_pending_` 留在 Runtime，清除后才排空 wake pipe 或 wake 事件；self-pipe 或 signal 事件创建失败降级，
wake pipe 或 wake 事件创建失败 terminate。
Terminal 的恢复与信号卸载顺序见 [tui-framework](tui-framework.md#41-终端l1)，构造所需状态在对应管道创建前分配。

exec 的跨平台 Asio/Boost.Process 对象和调度保留在共享文件，POSIX 启动钩子、原生管道与继承清单展开位于平台文件。
两条 spawn 路径独立，原创建、标记、关闭、移交顺序见 [exec](exec.md)。ProcessGroup 状态在 Runner/Child::Impl
构造阶段分配，早于创建子进程管道；其析构不发送信号。继承清单只保存元数据，不拥有资源，fork 前借用其中的 fd span。

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
`default_search_path` 返回 `/usr/bin:/bin`，`ReadOnlySandbox` 与 sandbox probe 用它作为受限命令的 PATH。
`project_search_path` 供 project 受限 Bash 使用：非空宿主 PATH 原样保留，未设置或空值时回退到
`/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`。project 宿主 Bash 和 HEAD diff 则通过
`base::environment_variable` 读取 PATH，缺失时不追加，空值时追加空 PATH，不使用六目录回退。

MXC 后端的库加载、策略 JSON、流读写线程、等待与终止以及回环代理都在共享的 `mxc.cpp`、`proxy.cpp`；
平台文件只提供三项：`SharedLibrary`（POSIX 为 `dlopen(RTLD_NOW | RTLD_LOCAL)`/`dlsym`，析构 `dlclose`）、
`sandbox_command_line`（POSIX 为 `exec setsid -w '<shell>' --noprofile --norc -c '<script>'`，sh 单引号转义，由 MXC 的 `/bin/sh -c` 解释；
`setsid -w` 让受限 shell 自成会话，命令里的 `kill 0` 不会杀到 bwrap，退出码原样回传）
和 `loopback_peer_is_current_user`（Linux 读 `/proc/net/tcp`，按对端端口找到客户端一侧的条目，比较 uid 与 `getuid()`；
找不到条目返回 false）。

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

`base::make_file_private` 提供抛异常及 error_code 两个重载：POSIX 直接执行原
filesystem::permissions(owner_read|owner_write)，Windows 设置受保护的当前用户/SYSTEM/Administrators DACL。
共享 runtime JSON 和 SearXNG 配置在原写入/关闭位置调用它，不改变失败传播与发布顺序。
配置保存原本通过 workspace 保留既有权限，未增加新的 chmod 时点；POSIX 决策库仍单独 best-effort chmod。
Windows `base/security_detail.hpp` 仅供平台文件使用，传递自相对描述符字节和校验结果，不暴露 Windows 头或句柄。
描述符由当前进程 token 用户 SID、SYSTEM 和 Administrators 构造；创建时指定当前用户所有、完全控制和受保护 DACL。
验证仅接受普通 allow ACE，必须禁止父 ACL 继承、无其他获准主体，当前用户具有读写权限（目录还需创建及遍历）。
NULL DACL、未保护 DACL、其他获准主体及未支持的 ACE 组合均拒绝，不通过忽略 ACE 推断为安全。

Windows SessionFileLock 与 SyncLock 在固定锁文件的 [0,1) 字节上非阻塞独占 LockFileEx；
ModelWriteLock 用同步句柄阻塞独占。所有锁句柄不继承，且打开期间不共享删除权限；锁文件不主动删除。
会话同进程租约注册表保持共享实现；Session/Model 锁析构 UnlockFileEx 后关闭，SyncLock 关闭释放。
凭据锁单独保留 30 秒期限、50ms 可取消等待和原错误分类，关闭锁文件后再关闭目录。
决策库通过 make_file_private(error_code) best-effort 收紧 DACL，调用位置不变。

Windows require_private_file 检查磁盘普通文件（拒绝目录/reparse point）和上述私有 DACL，
所有者必须是三个受信任主体之一；不安全时报告 invalid，并给出 Windows Security > Advanced 的所有者/继承/授权修复说明。
已有凭据则额外要求当前用户所有；目录与文件逐分量 NtCreateFile，拒绝 symlink/junction/ADS，
创建缺失目录、锁文件和独占临时文件时已经带受保护描述符，已有对象不自动修复。
凭据保持相同 JSON 编解码、尾部换行、4096 字节读取和 64 KiB 上限；
写入 FlushFileBuffers 后相对同目录 FileRenameInfoEx 替换，失败按临时句柄删除，删除使用 FileDispositionInfoEx。
Windows 目录 flush 为尽力而为，不声称具备 POSIX 目录 fsync 的持久性保证；不读取或修改特权 SACL。

Windows runtime_platform 返回 Windows-x86_64；tar 固定为系统目录的 tar.exe，保持 -xf/strip-components=<strip>/-C 参数，
strip 默认 1 并由 bsdtar 同时处理 .zip。runtime_environment 使用分号 PATH，包目录及变量按原迭代顺序排列，
每个包 bin 之后追加 manifest programs 中各程序父目录（去重），系统目录与 Windows 目录放在末尾；
Python 环境前置 Scripts，Node 前置 node_modules/.bin。HOME/USERPROFILE 指向所选受管 Home，
XDG/UV/NPM 变量沿用原含义，另显式传入 SystemRoot、系统 COMSPEC、系统临时目录 TEMP/TMP，不继承宿主 PATH。
install_python_runtime 返回 python/python3 程序映射；POSIX 仍创建并返回原两个链接，Windows 直接返回 uv find 的解释器路径。
select_python_runtime_program 在 POSIX 保留配置路径，在 Windows 选择安装器路径；配置相对路径仍在原位置验证。
程序校验、摘要、包/环境就绪标记和最终 manifest 发布顺序不变；Windows venv 程序为 Scripts/python.exe。
Windows 的系统 Bash/Git 与 uv/rg/node 归档由 home/config/runtime.json 的 platforms 覆盖登记，`mxc` 包的 Windows 覆盖
登记 `bin/x64/mxc_ffi.dll`；SearXNG 环境在该平台解析为 null，不准备。

以下锁与原生权限细节描述 POSIX 实现。

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

`host_prerequisites` 在 POSIX 检查 bwrap（`bwrap --version` ≥ 0.5）、slirp4netns、unshare、nsenter、setsid、ip、iptables-restore：
先走 exec::which，再检查 `/usr/bin`、`/usr/sbin` 下的固定位置；只返回路径与版本文本，不持有进程或文件资源。
Windows 返回空列表。AppArmor 诊断保持在真实 sandbox probe 之后追加，缺少 runtime 的提前返回不读取宿主诊断。
bubblewrap、slirp4netns 等进程由 MXC 原生库在后端进程内生成，不属于自有 C++ 的平台调用。
Git 命令参数及 unified diff 中的 `/dev/null` 是外部命令配置或文本格式，bash 分析中的设备路径是待分析命令的语义，
均不是共享 C++ 对本机原生 API 的调用。中立封装、标准流或跨平台库的 read/write/close 等同名方法也不等同于 fd 系统调用。

## Windows：工作区监视与沙箱能力报告

`app/queries_win.cpp` 为每个受监视目录保持一个 overlapped `ReadDirectoryChangesW`（非递归），由同一个完成端口在
watcher 线程中消费；目录筛选、4096 上限、不跟随 reparse point 目录与 POSIX 相同。新增或重命名进入的目录以及缓冲区溢出触发重扫，
其余事件触发 changed 回调；停止时投递完成键 0 唤醒，`CancelIoEx` 取消在途请求并收齐完成通知后才释放缓冲区和句柄。
Windows 没有 `IN_CLOSE_WRITE` 的精确对应物，通知可能在写入结束前或多次到达，契约只要求最终刷新正确。

`detail::platform_sandbox_supported()` 在两个平台都返回 true；`detail::sandbox_containment()` 在 POSIX 返回 `bubblewrap`，
Windows 返回 `processcontainer`。Windows 的 `sandbox_runtime` 在加载库后调用 `probe_request`（代理形状的请求），层级不是
`base-container` 或探测本身失败时返回原因（含层级名与 "Windows 11 26H2 (build 26300) or newer"），启动装配把它记入
`Support.missing`；`dagent sandbox status` 增加 `mxc-tier` 检查项与 `tier_probe` JSON。`append_host_diagnostics` 在 Windows
识别 MSYS2 初始化失败并给出 `host_prerequisite`，POSIX 保留 AppArmor 提示。

## Windows：终端

DAgent 作为 ConPTY 的客户端运行，不创建 ConPTY；终端由 Windows Terminal 或 ConPTY 捕获工具提供。
`terminal_win.cpp` 只使用控制台 API，公共头不包含 Windows 类型。

- 输入：构造时保存 `GetConsoleMode` 的输入模式，关闭 `ENABLE_LINE_INPUT`、`ENABLE_ECHO_INPUT`、
  `ENABLE_PROCESSED_INPUT`、`ENABLE_QUICK_EDIT_MODE`、`ENABLE_MOUSE_INPUT`，开启 `ENABLE_EXTENDED_FLAGS`、
  `ENABLE_WINDOW_INPUT`、`ENABLE_VIRTUAL_TERMINAL_INPUT`。`ReadConsoleInputW` 每次最多取 128 条记录，只取
  `bKeyDown` 且 `UnicodeChar != 0` 的 `KEY_EVENT`，按 `wRepeatCount` 重复；UTF-16 代理对跨记录、跨调用拼接，
  孤立代理丢弃，再以 `WideCharToMultiByte(CP_UTF8, 0, …)` 转为 UTF-8。VT 输入模式下方向键、功能键、鼠标与粘贴
  已由控制台转成 `\x1b[A`、`\x1b[<0;10;5M`、`\x1b[200~` 等序列逐字符送入，解码器直接识别，不翻译虚拟键码。
  `UnicodeChar == 0` 的按键、`MENU_EVENT` 与 `MOUSE_EVENT` 忽略；`FOCUS_EVENT` 转为解码器认识的 `\x1b[I` / `\x1b[O`；
  `WINDOW_BUFFER_SIZE_EVENT` 置待处理尺寸变化标志并唤醒事件循环。一次读入放不下的字节存入 `pending`。
- 输出：stdout 是控制台时保存输出模式，开启 `ENABLE_VIRTUAL_TERMINAL_PROCESSING` 与
  `DISABLE_NEWLINE_AUTO_RETURN`，并把 `GetConsoleOutputCP()` 的代码页改为 `CP_UTF8`。`write()` 用 `WriteFile`
  循环写满 UTF-8 字节，不使用 `WriteConsoleW`；stdout 重定向到文件或管道时同样按字节写出。
  进入/离开备用屏等序列仍由共享 `terminal.cpp` 生成。
- 事件与等待：手动重置的 `signal_event` 加 `quit_pending`、`resize_pending` 两个原子标志代替 POSIX 的 self-pipe；
  全局锚点供 atexit 与控制台事件处理器使用。`EventWait` 用 `WaitForMultipleObjects` 同时等待输入句柄、信号事件与
  手动重置的 wake 事件，超时为负表示 `INFINITE`；`pending` 非空或 stdin 不是控制台时不阻塞，后者让 `read_input`
  报告 closed，对应 Linux 下 stdin 为 `/dev/null` 的正常退出。信号事件或 wake 事件创建失败的处理与 POSIX 相同：
  signal 事件降级、wake 事件终止。
- 控制台事件：`SetConsoleCtrlHandler` 的处理器只接管 `CTRL_CLOSE_EVENT`、`CTRL_LOGOFF_EVENT`、`CTRL_SHUTDOWN_EVENT`，
  置退出标志、`SetEvent` 并直接 `restore()`（进程随后被结束），返回 TRUE；`CTRL_C_EVENT`、`CTRL_BREAK_EVENT`
  返回 FALSE 不接管：raw 模式关闭了 `ENABLE_PROCESSED_INPUT`，Ctrl-C 作为 0x03 键输入到达解码器，Ctrl-Break
  交给 `app/interrupts_win.cpp` 已注册的进程级处理。
- 与 POSIX 的差异：Ctrl-C 是键输入而非 SIGINT；没有 SIGHUP，关闭/注销/关机经由控制台事件退出；尺寸变化由
  `WINDOW_BUFFER_SIZE_EVENT` 报告而不是 SIGWINCH；`probe_caps()` 固定初始能力（真彩色、SGR 鼠标、括号粘贴为 true，
  同步输出、kitty 键盘、字素宽度为 false），不读 TERM/COLORTERM，后续由握手应答经 `apply_caps` 修正。
- 还原：`restore()` 仍由共享实现去重，调用 `restore_input()` 按已保存标志恢复输入模式、输出模式与输出代码页后清标志；
  析构顺序为 restore、卸载控制台处理器、关闭信号事件、清除全局实例。atexit 兜底与 POSIX 相同。

## TLS 吊销检查

net 层与 runtime 下载的 libcurl 句柄都设置 `CURLSSLOPT_REVOKE_BEST_EFFORT`：Windows 的 schannel 在吊销服务器不可达时（常见于只经代理出网的主机）
不再以 `CRYPT_E_REVOCATION_OFFLINE` 失败，证书本身的验证不变；OpenSSL 后端忽略该选项，Linux 行为不变。

## Windows：进程代码页

两个可执行文件嵌入 `cmake/dagent.manifest`（`activeCodePage=UTF-8`、`longPathAware`）。进程级 ANSI 代码页因此是 UTF-8，
共享代码里 `std::filesystem::path(std::string)`、`path.string()` 和 A 版 API 都按 UTF-8 解释窄字符串，与 `base::path_from_utf8/path_to_utf8` 的约定一致；
命令行参数经 CLI11 `ensure_utf8` 转为 UTF-8 后可直接构造路径。Linux 不嵌清单，行为不变。
