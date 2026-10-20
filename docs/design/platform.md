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

这是逐步落实的源码边界约定。当前已提取跨模块小原语与 workspace 的 OS 描述，尚未完成全仓 I1/I2：
IPC、TUI、exec、文件操作、信号、锁与监听等仍有共享实现中的平台代码；Channel、Terminal、Runtime、
Command、PipeFds 和 ModelWriteLock 仍含原生状态。下面只列已落地的平台文件。

## 当前平台文件与接口

| 平台文件 | 中立声明 | 实现 |
| --- | --- | --- |
| `src/private/base/platform_posix.cpp` | `src/public/base/platform.hpp` | `executable_path`、`home_directory`、`process_id`、`local_time`、`utc_time`、`utc_timestamp`、`can_execute`、`create_unique_directory` |
| `src/private/workspace/context_posix.cpp` | `src/public/workspace/context.hpp` | `detail::os_description` |

`executable_path` 的两个重载分别保留 filesystem 的抛异常和 error_code 行为，供 IPC、sandbox status、
SearXNG 定位程序资源。安装根 `app/paths.cpp` 的固定缓冲区 readlink 和 InstallError 仍保持独立。

`home_directory` 借用 getenv 的结果，不缓存、不规范化，保留 nullptr 与空字符串的区别。
`process_id` 返回当前进程号，调用位置与十进制格式不变。时间转换写入调用方的 `std::tm`，
local/UTC 转换返回是否成功，UTC 时间戳转换保留原返回值和对 tm 的修改；调用者维持各自的失败处理。

`can_execute` 只检查执行权限，不附加普通文件判断；exec 的 stat 检查仍在原位置。
`create_unique_directory` 接收完整 XXXXXX 模板，只调用 mkdtemp，成功返回目录，失败返回 strerror 文本。
父目录创建、固定 `/tmp` 选择、业务异常及清理由 SRT、MCP、SearXNG 各自保留。

`os_description` 返回 uname 的 `sysname release`，失败为 `unknown`。Toolchain 的平台标识与错误策略不同，
仍在原实现中。CLI/Terminal 的终端判断尚未提取；文件锁也未合并为 base 原语。
