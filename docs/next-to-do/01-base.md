# 01 base：日志与公共工具

库 `dagent_base`，命名空间 `dagent::base`。**最底层**：除 `app` 以外的每个模块都可以依赖它，它自己只依赖
标准库、spdlog 和 nlohmann。

| 子功能 | 头文件 | 里程碑 |
| --- | --- | --- |
| 日志 | `base/log.hpp` | M1 |
| dotenv 与密钥查找 | `base/dotenv.hpp` | M1 |
| 文本工具：UTF-8、截断、ANSI、base64 | `base/text.hpp` | M1（exec、workspace 在 M2 开始用） |
| JSON 脱敏 | `base/json.hpp` | M1 |

放进 base 的判断标准：**至少两个模块都需要、而且和业务无关**。只有一个模块用的东西，放在那个模块自己里面。

---

## 1. 日志（log.hpp）

### 职责

初始化全局日志，给每个模块提供具名 logger。**日志只写文件**：交互模式下终端归 TUI 所有，任何写到
stdout/stderr 的内容都会把界面弄花。本模块不再包一层日志 API，直接用 spdlog 的接口，只负责初始化和约定。

### 依赖

**spdlog**，通过 FetchContent 引入，写死最新的 release tag，**不加 `FIND_PACKAGE_ARGS`**：

```cmake
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)  # 用 std::format，不再依赖 fmt
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog GIT_TAG v1.x.y)
FetchContent_MakeAvailable(spdlog)
```

为什么不用系统包：系统的 spdlog 是基于 fmt 构建的，和 `SPDLOG_USE_STD_FORMAT` 不兼容。选它的理由：C++
日志库的事实标准，自带按大小滚动、按环境变量设置级别；开启 std::format 以后没有额外依赖。备选 quill 的
延迟更低，但 agent 的日志量用不上这个优势。

### 接口草图

```cpp
namespace dagent::base {
struct LogOptions {
    std::filesystem::path file;              // 默认 $XDG_STATE_HOME/dagent/logs/dagent.log
    std::size_t max_file_bytes = 5 << 20;
    std::size_t max_files = 3;
    std::string level = "info";              // 会被环境变量 DAGENT_LOG 覆盖
    bool also_stderr = false;                // 只在非交互模式下由 cli 打开（--verbose）
};
void init_log(const LogOptions&);
std::shared_ptr<spdlog::logger> logger(std::string_view module);   // "net"、"exec"……共用同一组 sink
void shutdown_log();                                               // 退出或崩溃时刷盘
}
```

`log.hpp` 里 include spdlog 是可以的，这是「第三方头文件不进公开头」规则的例外，因为调用方要直接调
`logger->info(...)`。

### 实现要点

- **所有 logger 共用同一组 sink**：先建好 `rotating_file_sink_mt`，`logger()` 创建时都挂到这组 sink 上，然后 `spdlog::register_logger`。不要每个模块各开一个文件。
- 格式：`[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [t%t] %v`。`%n` 是模块名，`%t` 是线程号，排查跨线程的时序问题时用得上。
- 刷盘策略：`flush_on(warn)` 加 `spdlog::flush_every(1s)`，不要每条都 flush。
- 级别：`DAGENT_LOG=debug` 或 `DAGENT_LOG=net=trace,info`，用 `spdlog::cfg::load_env_levels("DAGENT_LOG")` 直接解析。
- 崩溃时尽量保住日志：在 `std::set_terminate` 里先调 `shutdown_log()`。TUI 已经负责在崩溃时还原终端，两者不冲突。
- **没调 `init_log()` 之前**，`logger()` 返回一个 null sink 的 logger，这样各模块在 temp 检测程序里单独使用时不会崩溃。
- 日志目录不存在时自动创建。

---

## 2. dotenv 与密钥查找（dotenv.hpp）

### 职责

读取项目根的 `.env.dev` 和 `.env`，格式以仓库里的 [.env.example](../../.env.example) 为准。对外提供
「按变量名取值」的查找函数。

### 接口草图

```cpp
namespace dagent::base {
class Secrets {
public:
    // 依次读取这些文件，同一个键先读到的优先；文件不存在就跳过
    static Secrets load(std::span<const std::filesystem::path> files);
    // 先查进程环境变量，再查文件内容
    std::optional<std::string> get(std::string_view name) const;
private:
    std::unordered_map<std::string, std::string> values_;
};
std::vector<std::pair<std::string, std::string>> parse_dotenv(std::string_view text);
}
```

### 实现要点

- **不要调用 `setenv`，文件里的值只保存在 `Secrets` 这张表里。** 原因有两个：
  1. 写进进程环境以后，模型通过 bash 执行的每条命令都会继承这个值，只要执行 `echo $DEEPSEEK_API_KEY` 就能读到密钥。
  2. `setenv` 在多线程程序里本身就不安全。
- 解析规则：`KEY=VALUE`；`#` 开头的行是注释；值可以用单引号或双引号包裹，只有双引号里的 `\n` 做转义；可以带 `export ` 前缀；去掉首尾空白。自己写 20 行左右就够，不引库。
- `Secrets` 里的值不能出现在日志和异常信息里。

---

## 3. 文本工具（text.hpp）

exec 截断命令输出、workspace 读文件、解析 rg 的输出，都要用到这组函数，所以统一写在这里：

```cpp
namespace dagent::base {
bool is_valid_utf8(std::string_view);
std::string to_valid_utf8(std::string_view, bool* lossy = nullptr);   // 非法字节替换成 U+FFFD
std::size_t utf8_floor(std::string_view s, std::size_t pos);          // 从 pos 往前退到字符边界

struct Truncated { std::string text; std::size_t total_bytes; bool truncated; };
// 保留头部和尾部，中间换成 "\n…省略 N 字节…\n"；两处切点都落在 UTF-8 字符边界上
Truncated truncate_middle(std::string_view, std::size_t max_bytes);

std::string strip_ansi(std::string_view);          // 去掉 CSI（ESC [ …）、OSC（ESC ] … BEL/ST）以及单字符 ESC 序列
std::string base64_decode(std::string_view);       // rg --json 的 {"bytes": …} 字段要用
}
```

- UTF-8 校验要拒绝过长编码（overlong）、代理区（U+D800–DFFF）和超过 U+10FFFF 的码点。用状态机实现，大约 30 行。
- `truncate_middle` 头尾各分一半的预算。输出末尾的报错信息通常最重要，所以不能只保留头部。
- `strip_ansi` 也要处理被截断在字符串末尾的半个转义序列，直接丢弃即可。

---

## 4. JSON 脱敏（json.hpp）

```cpp
namespace dagent::base {
// 递归处理对象和数组，把键名（忽略大小写）在 fields 里的值替换成 "***"
void redact(nlohmann::json& j, std::span<const std::string> fields);
}
```

session 写盘之前、log 输出请求体之前都要调用它。

---

## 验收（temp/base_check）

1. 多个线程、多个模块 logger 并发写同一个文件，每一行都完整，没有交错。
2. 写满后按大小滚动，文件数不超过 `max_files`。
3. `DAGENT_LOG=net=debug,warn` 下，net 的 debug 日志会输出，其他模块的 info 日志被过滤掉。
4. 进程 `abort()` 时，最后一条 warn 日志已经在文件里。
5. `Secrets`：`.env.dev` 和 `.env` 里有同一个键时，取 `.env.dev` 的值；进程环境里已经有同名变量时，取环境变量的值；加载以后，子进程 `sh -c 'echo $DEEPSEEK_API_KEY'` 的输出为空。
6. UTF-8：用 Markus Kuhn 的 UTF-8 解码压力测试文件（UTF-8-test.txt）跑一遍，每一类非法序列都能被识别出来。
7. `truncate_middle` 对一段 10 MB 的中文文本截断，结果是合法的 UTF-8，`total_bytes` 正确。
8. `strip_ansi` 处理 `ls --color=always`、`git -c color.ui=always log` 的真实输出后，不再残留任何 ESC 字符。
9. `redact` 能处理嵌套对象和数组里的 `Authorization`、`api_key`（大小写混用）字段。

## 审核关注点

交互模式下有没有任何地方写 stderr；sink 是否真的共享；`Secrets` 是否完全没有调用 setenv；UTF-8 边界切分。
