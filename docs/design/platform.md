# platform：跨平台编译期边界与实现策略

DAgent 同时支持 **Linux（`posix`）** 与 **Windows（`win`）**。为保持核心与业务层代码的纯粹性，跨平台适配采用**编译期源文件选择 + 不透明资源句柄**的设计，不在业务层散布 `#ifdef` 宏，也不引入多余的运行时虚函数开销。

## 1. 架构隔离原则

1. **公共头文件绝对中立**：`src/public/**` 下绝不包含 `<unistd.h>`、`<sys/*>`、`<windows.h>` 等平台原生头文件，也不在公开结构体或接口中暴露 `fd`、`pid_t`、`HANDLE`、`termios` 等原生类型（底层状态统一封装在 Pimpl 或不透明句柄中）。
2. **编译期静态分发**：根 CMake 根据目标系统定义 `DAGENT_PLATFORM`（`posix` 或 `win`），在构建时自动将 `src/private/<模块>/<功能>_${DAGENT_PLATFORM}.cpp` 编入对应模块。
3. **对外统一 UTF-8**：所有跨模块边界的路径、环境变量、进程参数与文件读写在公共接口层统一使用 UTF-8 `std::string` / `std::filesystem::path`；Windows 平台实现内部在调用 Win32/NT API 前后完成 UTF-8 与 UTF-16 的严格无损转换，并通过内嵌程序清单（Manifest）将进程活动代码页固定为 UTF-8 且启用长路径支持。

## 2. 核心子系统的双平台机制对照

| 子系统 | Linux (`posix`) 实现机制 | Windows (`win`) 实现机制 |
| --- | --- | --- |
| **前后端私有 IPC** | `socketpair` + `fork`/`exec`；子进程固定继承 `fd 3` 并用 `close_range` 关闭其余描述符；独立进程组；超时 `SIGTERM` → `SIGKILL` | 当前用户私有 DACL 的预连接双向命名管道（`OVERLAPPED` 异步 I/O）；`STARTUPINFOEX` 显式句柄白名单；绑定 `KILL_ON_JOB_CLOSE` 生命周期 Job 对象 |
| **子进程与进程组** | `setsid` 创建无控制终端的新会话；超时/取消对 `-pid` 整个进程组发送信号；忽略全局 `SIGPIPE` | 新建控制台进程组并加入独立 `KILL_ON_JOB_CLOSE` Job；终止时先尝试 `Ctrl-Break`，宽限期后调用 `TerminateJobObject` 清理整棵进程树；针对 Git for Windows（MSYS2）`bash.exe` 专门实现全反斜杠参数转义（`msys_quote`）与虚拟盘符映射（`/c/...` ↔ `C:\...`） |
| **防篡改文件原语** | `O_PATH` + `openat2`（禁止穿越新符号链接）固定目录与文件 inode；`renameat2`（新建带 `RENAME_NOREPLACE`）原子提交并 `fsync` 目录 | `NtCreateFile` 基于 `RootDirectory` 逐级打开并拒绝 Reparse Point / ADS；校验卷序列号与 128 位 File ID；`FileRenameInfoEx`（`POSIX_SEMANTICS`）原子替换 |
| **私有文件与凭据保护** | 校验普通文件、当前有效 UID 所有权及 `0600`（目录 `0700`）权限位；逐级 `openat(..., O_NOFOLLOW)` | 校验并设置受保护的私有 DACL（禁止父目录继承，仅授权当前用户、SYSTEM 与 Administrators 完全控制） |
| **跨进程文件写锁** | `flock(LOCK_EX)`（会话锁与工具链同步锁非阻塞，模型配置锁阻塞，凭据锁带可取消轮询） | 在锁文件 `[0, 1)` 字节区间调用 `LockFileEx(LOCKFILE_EXCLUSIVE_LOCK)`，禁止共享删除，句柄关闭自动释放锁 |
| **工作区文件监听** | `inotify` 监听最多 4,096 个目录 + `eventfd` 取消唤醒 | 每个目录投递异步 `ReadDirectoryChangesW`，由单一 I/O 完成端口（IOCP）在监听线程集中消费 |
| **受限执行沙箱（MXC）** | 动态加载 `libmxc_ffi.so`，基于 `bubblewrap`（user/mount/pid/net 命名空间，启用非特权 userns 限制的发行版需加载构建生成的 `dagent.apparmor` profile）+ `slirp4netns` + `iptables` 落实隔离 | 动态加载 `mxc_ffi.dll`，基于 Windows `ProcessContainer`（`base-container` 层级）；*注：当前因上游 MSYS2 `bash` 无法在容器内初始化命名对象，探针自动触发 Fail-Closed 回退为无沙箱模式* |

## 3. Windows：终端（ConPTY 客户端）

在 Windows 上，DAgent 作为 Windows Terminal 等现代终端（ConPTY）的客户端运行，`terminal_win.cpp` 将控制台机制无缝对接到共享 TUI 框架：

- **输入管线**：开启 `ENABLE_VIRTUAL_TERMINAL_INPUT` 与窗口事件，通过 `ReadConsoleInputW` 读取按键、VT 控制序列（方向键、鼠标 SGR、括号粘贴）、焦点切换与窗口尺寸变化（`WINDOW_BUFFER_SIZE_EVENT`），将 UTF-16 代理对拼合转为 UTF-8 字节流送入共享 `Decoder`。
- **输出管线**：开启 `ENABLE_VIRTUAL_TERMINAL_PROCESSING` 并将控制台输出代码页设为 `CP_UTF8`，直接复用共享层的 ANSI/VT 差分渲染帧字节流。
- **事件多路等待**：使用 `WaitForMultipleObjects` 同时等待控制台输入句柄、退出信号事件与跨线程 `wake` 事件（替代 POSIX 的 `poll` + `self-pipe`）。
