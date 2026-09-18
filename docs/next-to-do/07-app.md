# 07 app：配置与命令行（入口层）

库 `dagent_app`，命名空间 `dagent::app`。**位于最顶层**：它要认识所有模块的 Options 结构，所以可以依赖
所有模块，但不能有任何模块反过来依赖它。里程碑 **M1**。

| 子功能 | 头文件 | 里程碑 |
| --- | --- | --- |
| 配置加载 | `app/config.hpp` | M1（各模块的 Options 做出来一个就映射一个） |
| 命令行解析 | `app/cli.hpp` | M1 |

程序入口 `src/private/app/main.cpp`（可执行目标 `dagent`）也放在这个目录。**入口怎么组装由你来写**：拿到
`Args` 和 `Config` 以后，怎么构建 UI、agent 和各模块，属于核心。

log 和 config 不能放在同一个库里：config 要 include 所有模块的 Options，如果它和 log 同在 base，就会形成
base → exec → base 的环。

## 依赖

| 用途 | 依赖 | 引入方式 |
| --- | --- | --- |
| 配置 | nlohmann/json（已有），`base::Secrets` | — |
| 命令行 | **CLI11** | FetchContent，写死 tag，加 `FIND_PACKAGE_ARGS`；header-only |

用到 nlohmann 的三个能力：`json::parse(text, nullptr, true, /*ignore_comments=*/true)` 允许配置里写 `//` 注释；
`json::merge_patch` 按 RFC 7386 做分层合并（对象递归合并，数组整体替换，`null` 表示删除这个键）；
`json::flatten()` 用来检测未知键。

CLI11 支持子命令、互斥选项和环境变量回退。备选的 argparse (p-ranav) 对子命令的支持比它弱。

---

## 1. 配置（config.hpp）

### 职责

把分散在多处的配置合并起来，映射成强类型的 `Config`。**只有这个模块知道 JSON 里的键名**，其他模块只认
自己的 Options。

不做：热重载；配置的写回。

### 配置来源与优先级（从低到高）

| 层 | 位置 | 说明 |
| --- | --- | --- |
| 1 内置默认 | 各 Options 结构的默认值 | 没有任何配置也能跑 |
| 2 用户级 | `$XDG_CONFIG_HOME/dagent/config.json`（默认 `~/.config/dagent/config.json`） | 个人偏好 |
| 3 项目级 | `<项目根>/.dagent/config.json` | 项目根就是 git 根，没有 git 就用 cwd |
| 4 显式文件 | `--config <文件>` | 指定之后**替换**第 2、3 层，不参与合并 |
| 5 命令行 | `--set gateway.model=xxx`、`-m xxx` | 优先级最高 |

仓库里现有的 `config/dagent.json` 属于开发期的第 4 层：开发时传 `--config config/dagent.json` 使用。

### 凭据

**配置文件里不写密钥本身**，只写密钥所在的变量名：`"gateway": {"api_key_env": "DEEPSEEK_API_KEY"}`。

取值通过 `base::Secrets` 完成：先查进程环境变量，再查项目根的 `.env.dev`、`.env`（格式见
[.env.example](../../.env.example)）。现有的 `network.credentials` 字段解释为「主机名 → 变量名」，例如
`{"api.deepseek.com": "DEEPSEEK_API_KEY"}`。

### 接口草图

```cpp
namespace dagent::app {

struct Gateway {
    std::string base_url, model;
    int max_tokens = 4096;
    std::optional<double> temperature;
    bool enable_thinking = false;
    std::filesystem::path system_prompt_file;   // 已解析成绝对路径
    std::string api_key;                        // 已经从 Secrets 里取出；可能为空（本地网关）
};

struct Config {
    Gateway gateway;
    net::HttpOptions http;
    exec::Options process;
    workspace::FileOptions files;
    session::Options session;
    base::LogOptions log;
    // context / run / permissions 这几段的类型由核心定义，config 只负责映射
    std::vector<std::filesystem::path> sources; // 实际参与合并的文件，按优先级从低到高
};

struct LoadOptions {
    std::filesystem::path cwd;
    std::optional<std::filesystem::path> explicit_file;
    std::vector<std::string> overrides;         // "a.b.c=value"
};

Config load_config(const LoadOptions&, const base::Secrets&);

class ConfigError : public std::runtime_error { /* Kind: parse / type / io */ };
}
```

### 实现要点

- **相对路径相对于它所在配置文件的目录**，不是相对 cwd。现有的 `"system_prompt_file": "../prompts/file_agent.txt"` 就依赖这个规则。所以要在**合并之前**把每一层里的路径字段先解析成绝对路径，否则合并以后就不知道某个路径是从哪个文件来的。
- **未知键只警告、不报错**：把合并结果 `flatten()` 后和已知键表对比，多出来的键打一条 warn 日志，比如「未知配置项 gateway.modle」。
- **类型错误要报出完整路径**：nlohmann 的 `type_error` 不说是哪个键出的错。catch 住以后，重新抛一条带 JSON 指针的 `ConfigError`，比如 `/http/timeout_seconds 应为数字`。
- 键名到字段的映射**逐项手写**（例如 `timeout_seconds` → `std::chrono::seconds`）。不要用 `NLOHMANN_DEFINE_TYPE_*` 宏，因为键名、字段名和单位都对不上。
- `--set` 的值先尝试按 JSON 解析（`true`、`3`、`"x"`、`[1,2]`），失败就当字符串，所以 `--set gateway.model=qwen` 不用写引号。
- `api_key` 不能出现在任何日志和错误信息里。

---

## 2. 命令行（cli.hpp）

### 命令设计（参考 codex / opencode）

```
dagent [选项] [提示词]              进入交互界面；给了提示词就把它作为第一条消息
dagent run [选项] <提示词>          非交互：跑完一轮后退出，输出写到 stdout
dagent sessions                     列出最近的会话
dagent --version

通用选项：
  -C, --cwd <目录>                  工作目录（默认当前目录）
  -c, --config <文件>               显式配置文件
  -m, --model <模型>                等价于 --set gateway.model=<模型>
      --set <键=值>                  覆盖配置，可以重复
  -r, --resume <会话ID>             恢复指定会话
      --continue                    恢复本项目最近的一次会话
      --log-level <级别>

run 专用：
      --permissions <auto|deny>     没有人审批时的策略
      --output <text|json|jsonl>    jsonl 表示逐个输出事件，给脚本和 CI 用
```

### 接口草图

```cpp
namespace dagent::app {
enum class Mode { interactive, run, sessions };
enum class OutputFormat { text, json, jsonl };

struct Args {
    Mode mode = Mode::interactive;
    std::string prompt;                     // 已经合并了 stdin 的内容
    std::filesystem::path cwd;
    std::optional<std::filesystem::path> config_file;
    std::vector<std::string> overrides;     // -m 已经转换成 gateway.model=…
    std::optional<std::string> resume_id;
    bool continue_last = false;
    std::optional<std::string> log_level;
    std::string permissions = "auto";
    OutputFormat output = OutputFormat::text;
};

// --help、--version 和解析错误会直接打印并返回退出码；其他情况返回 Args
std::variant<Args, int> parse_args(int argc, char** argv);
}
```

### 实现要点

- **从 stdin 读提示词**：`!isatty(STDIN_FILENO)` 时读完整个 stdin。如果同时给了提示词参数，就拼成「参数 + 空行 + stdin 内容」，这样支持 `git diff | dagent run "审查这个改动"`。
- **交互模式要求 stdin 是终端**：如果 stdin 被重定向了，就提示「非交互场景请使用 `dagent run`」并退出。
- 退出码：0 成功；1 运行失败（模型报错、达到调用上限）；2 参数错误（CLI11 的默认值）；130 被 Ctrl+C 中断。把这些写进 `--help` 的末尾。
- `-C` 在解析阶段转成绝对路径，但**不要在这里 chdir**。
- `-m` 和 `--set gateway.model` 同时出现时，按出现顺序处理，后面的覆盖前面的。
- 目录类参数用 CLI11 的 `->check(CLI::ExistingDirectory)` 校验，不要自己写。

---

## 验收（temp/app_check）

**config**

1. 三层合并：用户级设了 model，项目级设了 max_tokens，`--set` 覆盖了 model，最终结果符合预期。
2. 指定 `--config` 以后，用户级和项目级的配置都不生效。
3. 相对路径按所在文件的目录解析，从不同 cwd 启动结果一致。
4. 写错一个键名，日志里出现警告；类型写错时，报错信息里带 JSON 指针路径。
5. 带 `//` 注释的配置能正常加载。
6. `api_key_env` 指向的变量能从 `.env.dev` 取到；进程环境里已经有同名变量时，以环境变量为准。

**cli**

7. 上面列出的每种写法都能解析成正确的 `Args`，包括子命令和选项顺序打乱的情况。
8. `echo hi | dagent run "说"` 得到的 prompt 是 `说\n\nhi`。
9. 交互模式下 stdin 被重定向时，给出提示并以退出码 2 退出。
10. `--help` 输出完整，中文不乱码。

## 审核关注点

路径解析的时机（必须在合并前）；密钥有没有泄漏；数组的合并语义（整体替换）；stdin 的判断。
