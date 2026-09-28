# base：日志与公共工具

DAgent 外围模块的最底层。头文件在 `src/public/base/`，实现在 `src/private/base/`，构建为静态库
`dagent_base`（公开链接 `spdlog::spdlog`）。命名空间 `dagent::base`。

除入口层 app 以外的每个外围模块都可以依赖它；它自己只依赖标准库、spdlog 和 nlohmann/json。
放进 base 的标准：至少两个模块需要，并且和业务无关。

---

## 1. 概述

| 能力 | 头文件 | 主要接口 |
| --- | --- | --- |
| 日志：全局初始化、按模块命名的 logger、滚动文件、崩溃时刷盘 | `base/log.hpp` | `init_log`、`logger`、`shutdown_log` |
| 文本处理：UTF-8 校验与修复、按字符边界截断、清理 ANSI 转义、base64 解码 | `base/text.hpp` | `to_valid_utf8`、`truncate_middle`、`strip_ansi`、`base64_decode` |
| JSON 参数解析与脱敏 | `base/json.hpp` | `parse_arguments`、`require_string`、`get_string`、`get_int`、`get_bool`、`redact` |

核心控制动作和普通工具共用 JSON 参数解析规则，业务 Schema 校验仍由各自模块负责。

约定：

- `truncate_middle` 直接返回截断后的字符串；截断标记保留省略字节数。`exec::Result.out/err` 同样直接保存字符串。
- **日志只写文件。** 交互模式下终端归 TUI 所有，写到 stdout/stderr 的内容会把界面弄花。

---

## 2. 日志

### 接入方式

入口在启动时调用一次 `init_log`，退出前调用 `shutdown_log`；各模块在**用到的地方**调用 `logger("模块名")`：

```cpp
base::init_log({.file = {}, .level = "info"});          // 程序启动时
base::logger("net")->debug("POST {} → {}", url, status); // 模块内部
base::shutdown_log();                                    // 程序退出前
```

- 所有 logger 共用同一组 sink：一个按大小滚动的文件，另外在 `also_stderr` 打开时加一个 stderr sink（只在非交互模式下使用）。
- 调用 `init_log` 之前，`logger()` 返回一个什么都不输出的 logger，所以各模块在 temp 检测程序里单独运行时不会崩溃。
- **不要在静态初始化阶段缓存 logger**。在 `init_log` 之前拿到的是 null logger，之后也不会自动更新；重新 `init_log` 以后，之前缓存的 logger 仍然挂在旧的 sink 上。
- spdlog 的默认接口（`spdlog::info` 等）也被接到同一组 sink 上，不会写到终端；`shutdown_log` 之后，默认 logger 会换成什么都不输出的 logger。

### 选项与行为

| `LogOptions` 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `file` | 入口设置为 `<root>/logs/dagent-<pid>.log` | 必填；所在目录不存在时自动创建 |
| `max_file_bytes` | 5 MiB | 单个文件的上限，写满后滚动 |
| `max_files` | 3 | 日志文件**总数**的上限，包括当前正在写的文件 |
| `level` | `info` | 全局默认级别 |
| `also_stderr` | `false` | 同时输出到 stderr |

- 格式：`[2026-09-18 10:00:00.123] [模块] [级别] [t线程号] 内容`。
- 刷盘：warn 及以上级别立即刷盘，其余每秒刷一次。`std::terminate` 时会先刷盘，再交给原来的处理函数。
- 级别解析失败时，`init_log` 抛 `std::invalid_argument`。

### 用环境变量 DAGENT_LOG 设置级别

`DAGENT_LOG` 会覆盖 `LogOptions::level`。写法是用逗号分隔的若干项：不带 `=` 的项设置默认级别，`模块=级别`
单独设置某个模块的级别。各项的顺序不影响结果。

```bash
DAGENT_LOG=debug                  # 所有模块 debug
DAGENT_LOG=net=debug,warn         # net 模块 debug，其余模块 warn
```

级别名：`trace`、`debug`、`info`、`warn`、`error`、`critical`、`off`。

---

## 3. 文本工具

| 接口 | 行为 |
| --- | --- |
| `is_valid_utf8` | 严格校验：过长编码、代理区码点（U+D800–DFFF）、超过 U+10FFFF 的码点都判为非法 |
| `to_valid_utf8(s, &lossy)` | 每个非法字节替换成一个 U+FFFD，`lossy` 记录是否发生过替换 |
| `utf8_floor(s, pos)` | 把 `pos` 往前退到字符边界 |
| `truncate_middle(s, max)` | 超过上限时保留头部和尾部（各占一半预算），中间换成 `\n…省略 N 字节…\n`；切点都落在字符边界上。标记不计入预算，所以只超出几个字节时，结果可能比原文略长 |
| `strip_ansi` | 清理 CSI、OSC 以及 DCS/APC/PM/SOS 字符串序列、带中间字节的 ESC 序列（比如 `ESC ( B`）。不完整的序列只丢掉引导字节，后面的正文保留；被截在末尾的半个序列整段丢弃 |
| `base64_decode` | 标准字母表，允许 CR/LF 换行，遇到 `=` 就结束；出现非法字符时抛 `std::invalid_argument` |

---

## 4. JSON 脱敏

`redact(json, fields)` 递归处理对象和数组，把键名（ASCII 比较，忽略大小写）**完全等于** `fields` 中某一项
的值替换成 `"***"`。它只按键名匹配，所以以键值对数组形式存放的 HTTP 头，或者 `x-api-key` 这种不在列表里的
键名，都需要调用方自己处理。

---

## 5. 依赖与构建

- spdlog v1.17.0，在 `cmake/deps.cmake` 里通过 FetchContent 从源码构建，并开启 `SPDLOG_USE_STD_FORMAT`（使用 std::format，不依赖 fmt）。不使用系统的 spdlog 包，因为它是基于 fmt 构建的，与这个选项不兼容。
- 在 CMake 之外链接 spdlog 的程序，需要加上 `-DSPDLOG_COMPILED_LIB -DSPDLOG_USE_STD_FORMAT`（完整的链接写法见 [docs/README.md](../README.md) 的「临时检测程序」）。
