# tools：工具层

库 `tools`（CMake target 不带 `dagent_` 前缀），命名空间 `dagent::tools`。头文件在 `src/public/tools/`，
实现在 `src/private/tools/`。依赖 base、exec、workspace、mcp；**不依赖 agent**（核心依赖它，反过来会成环）。

## 职责

把外围模块包装成模型能调用的工具：定义名字、说明和参数 Schema，解析并校验模型给的参数，调用外围模块，
把结果整理成两份——**给模型的文本**和**给界面与会话的结构化数据**。

判断标准和外围相反：这里放的是「面向模型的语义」。read 要不要带行号、edit 怎样匹配、bash 的输出怎样
呈现给模型，都在这一层；「执行一条命令、截断输出」仍然在 exec。

**不做**：

- 权限决策。工具只**陈述**自己打算做什么（`Intent`），允许、询问还是拒绝由核心决定。
- 调度。哪些调用并行、调用上限、失败后是否继续，由核心决定。
- 消息历史。结果怎样放进 `agent::Message`、要不要压缩，由核心决定。

## 和核心的边界

```
模型给出 ToolCall{id, name, arguments}
  │
  ▼ registry.find(name)           找不到 → 核心自己构造错误结果
  ▼ tool.prepare(arguments, ctx)  解析、校验、预演；参数有问题 → 直接得到 is_error 的 Result
  │                               成功 → Call，带 Intent（路径、命令、diff 预览……）
  ▼ 核心：按 Intent 做权限决策     拒绝 → 核心构造「用户拒绝」结果
  ▼ call.run(grant, on_output, stop)
  ▼ Result{text → 模型, display → 界面/会话}
```

**两阶段**的原因：权限对话框需要在执行前看到「要改什么」（edit 的 diff、bash 的命令和只读分析），而且
一个注定失败的调用（找不到 old_string、参数缺字段）不应该先弹一次确认。`prepare` 没有副作用，可以读文件，
但不写任何东西。

## 接口草图

```cpp
namespace dagent::tools {

/// dagent.json 的 "tools" 段（新增，由 app 映射）。
struct Options {
    std::size_t max_result_bytes = 32 << 10;   ///< 每次调用交给模型的文本上限（约 8k token）
    int read_default_lines = 2000;
    std::size_t read_max_line_bytes = 2000;    ///< read 输出里单行的上限，超出截断
    std::size_t grep_max_matches = 200;
    std::size_t glob_max_files = 200;
    std::chrono::milliseconds bash_max_timeout{600000}; ///< 模型能要求的最长超时
};

/// 给模型的工具描述。核心把它转成 agent::ToolDef（字段一一对应）。
struct Spec {
    std::string name, description;
    nlohmann::json parameters;                 ///< JSON Schema
};

/// 工具打算做什么：权限决策的输入，只描述，不决策。
struct Intent {
    enum class Kind { read, write, exec, external };  ///< external：MCP 工具，语义未知
    Kind kind = Kind::read;
    std::vector<workspace::Resolved> paths;   ///< read/write 涉及的路径，带 inside_workspace
    std::string command;                      ///< exec：原始命令
    bool known_readonly = false;              ///< exec：exec::is_known_readonly 的结果
    std::string preview;                      ///< write/edit：unified diff，给确认对话框
    std::string summary;                      ///< 一行描述，如「编辑 src/a.cpp（+3 −1）」
};

/// 核心的决定，执行时传回。
struct Grant {
    exec::Mode sandbox = exec::Mode::workspace_write; ///< 只对 bash 有意义
    bool allow_network = false;
};

struct Result {
    std::string text;         ///< 给模型：合法 UTF-8，已按 max_result_bytes 截断
    bool is_error = false;    ///< 模型视角的失败：参数错、找不到、匹配失败、退出码非 0……
    bool interrupted = false; ///< stop_token 触发；text 里是已有的部分输出
    View display;             ///< 给界面与会话，见「界面数据：View」一节
};

/// 会话级状态：核心每个会话建一个，所有调用共用。线程安全。
class Context {
public:
    Context(std::filesystem::path root, Options, workspace::FileOptions, workspace::SearchOptions,
            exec::Options);
    const std::filesystem::path& root() const;   ///< 工作区根，即 Args::cwd
    // 内部持有 FileTracker：模型读过/写过的文件 → 当时的 Stamp（见 edit/write）
};

class Call {
public:
    virtual ~Call() = default;
    virtual const Intent& intent() const = 0;
    /// 在调用线程上阻塞执行；on_output 只有 bash 会调用（原始输出块）。
    /// 不抛异常：取消返回 interrupted=true，其他失败都是 is_error 的 Result。
    virtual Result run(const Grant&, const std::function<void(std::string_view)>& on_output,
                       std::stop_token) = 0;
};

class Tool {
public:
    virtual ~Tool() = default;
    virtual const Spec& spec() const = 0;
    /// 解析、校验、预演。参数有问题时返回 is_error 的 Result。没有副作用。
    virtual std::expected<std::unique_ptr<Call>, Result> prepare(std::string_view arguments,
                                                                 Context&) const = 0;
};

class Registry {
public:
    void add(std::unique_ptr<Tool>);                 ///< 同名替换
    void remove_prefix(std::string_view prefix);     ///< 刷新某个 MCP server 前移除 mcp__<server>__
    const Tool* find(std::string_view name) const;
    std::vector<const Spec*> specs() const;          ///< 按注册顺序，保证每次请求里顺序稳定
};

void add_builtin(Registry&);                          ///< read / write / edit / bash / grep / glob
void add_mcp(Registry&, mcp::Client&);                ///< Client 要比这些工具活得久

}
```

**run 不抛异常**是有意的：核心对每个调用只需要处理一种返回值。cancel 时 bash 已经有部分输出，放进
`interrupted` 的结果里比抛异常丢掉更有用；环境问题（rg 没装、沙箱准备失败、MCP 断连）模型修不了，但也
应该知道，同样作为 `is_error` 的结果返回，同时记 warn 日志。

## 界面数据：View

`Result::display` 给界面用，也要存进会话，供 `--resume` 时重画历史。每个工具一个结构体，放在
`tools/view.hpp`：

```cpp
struct ReadView {
    std::string path;
    int start_line = 0, end_line = 0, total_lines = 0;
    bool truncated = false, directory = false;
};
struct FileChangeView {                 // edit 和 write 共用，界面按同一种方式画 diff
    std::string path, diff;             // diff 是 unified_diff 的文本
    int added = 0, removed = 0;
    bool created = false;
};
struct BashView {
    std::string command, output;        // output 是 exec 截断后的完整输出，不是给模型的那份
    std::optional<int> exit_code, signal;
    bool timed_out = false, interrupted = false;
    std::string sandbox;                // "read_only" / "workspace_write" / "full_access"
    std::int64_t elapsed_ms = 0;
};
struct GrepLine {
    std::string path, text;
    std::uint64_t line = 0;
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    bool is_context = false;
};
struct GrepView { std::string pattern; std::vector<GrepLine> lines; bool truncated = false; };
struct GlobView { std::string pattern; std::vector<std::string> files; bool truncated = false; };
struct McpView {
    std::string server, tool;
    std::vector<nlohmann::json> content; // 内容块原样保留，界面自己决定怎样显示
    nlohmann::json structured;
    bool disconnected = false;
};

/// monostate：prepare 阶段就失败的调用（参数错误等），界面只显示 text。
using View = std::variant<std::monostate, ReadView, FileChangeView, BashView, GrepView, GlobView, McpView>;

nlohmann::json to_json(const View&);          // {"kind": "change", ...}，给 session::Writer::append
View view_from_json(const nlohmann::json&);   // 回放时用；kind 不认识时返回 monostate
```

- 每个结构体用 `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT` 声明序列化，缺字段时取默认值：以后给结构体
  加字段，旧会话照样读得出来。
- `std::variant` 本身 nlohmann 不支持，`to_json`/`view_from_json` 手写，用 `kind` 字段区分：`read`、`change`、
  `bash`、`grep`、`glob`、`mcp`。
- 界面用 `std::visit` 处理，漏掉哪种 View 编译时就能发现。

## 各工具

所有路径参数：相对路径基于 `ctx.root()`；开头的 `~/` 展开成 `$HOME`；统一经 `workspace::resolve`。

### read

参数：`path`，`offset`（从 1 开始的行号，可选），`limit`（行数，默认 `read_default_lines`）。

- 输出每行 `行号\t内容`，行号不补空格。单行超过 `read_max_line_bytes` 截断并标 `…`。
- 结尾按情况补一行说明：`[文件共 N 行，已显示 a–b 行，用 offset 继续读]`；空文件输出 `（空文件）`；
  `lossy` 时说明「含非法 UTF-8，已替换为 U+FFFD」。
- 二进制：`is_error`，说明大小，不输出内容。不存在：`is_error`，并用 `files` + `fuzzy_rank` 给出至多 3 个
  相近路径。**目录**：列出直接子项（目录名带 `/`，上限 `glob_max_files`），省掉单独的 ls 工具。
- 读到的 Stamp 记进 FileTracker（部分读取也算）。
- display：`ReadView`——界面只显示「读取 a.cpp 1–200 行」，不显示内容。

### edit

参数：`path`，`old_string`，`new_string`，`replace_all`（默认 false）。

- **必须先 read**：FileTracker 里没有这个文件 → `is_error`「先用 read 读这个文件」。
- **stale**：`prepare` 读文件时 Stamp 和 FileTracker 里的不一致 → `is_error`「文件在你上次读取后被修改过
  （可能是 bash 或用户改的），请重新 read」。`run` 写入时再用 `write_text(expect=…)` 兜底，覆盖 prepare 到
  run 之间用户思考的那段时间。
- 匹配在 LF 内容上做；`old_string`/`new_string` 先统一成 LF。写回时保留原文件的 eol 和 bom。
- **只做精确匹配**。0 处 → `is_error`；多处且没开 `replace_all` → `is_error`，列出各处的行号。
  `old_string` 为空、`old_string == new_string`、替换后内容不变 → `is_error`。
- 失败时的提示要能让模型自己修好：`old_string` 每行都以「数字 + Tab」开头时，提示「不要包含 read 输出的
  行号前缀」；忽略行首缩进后能唯一匹配时，指出行号并提示「缩进不一致」——只提示，不自动应用。
- **lossy 的文件拒绝编辑**：替换过 U+FFFD 的内容写回去会破坏原来的字节。
- `prepare` 里算好新内容和 `unified_diff`，放进 `Intent::preview`；`run` 只负责写。
- 成功后给模型：`已编辑 path（+a −b）`，加上改动处前后各 4 行（带行号），多处时总量按预算截断。
  FileTracker 更新为新 Stamp，连续编辑不需要重新 read。
- display：`FileChangeView`（`created = false`）。

### write

参数：`path`，`content`。

- 文件已存在：同 edit，必须先 read、做 stale 检查；保留原文件的 eol 和 bom。文件不存在：直接创建（父目录
  自动建），LF、无 BOM。`content` 先统一成 LF。
- 超过 `max_write_bytes` → `is_error`。
- 给模型：`已创建 path（N 行）` / `已覆盖 path（+a −b）`，不回显内容。
- display：`FileChangeView`。

### bash

参数：`command`，`timeout_ms`（可选，上限 `bash_max_timeout`，默认 `process.default_timeout`）。

- 执行 `bash -c <command>`，cwd 是 `ctx.root()`，`merge_stderr = true`，stdin 接 `/dev/null`。
- **每次调用都是新进程**：`cd`、`export` 不保留。说明里要告诉模型：需要换目录就写 `cd dir && …`。
- **沙箱**：按 `Grant` 调 `exec::prepare`，`writable = {root, /tmp}`；`full_access` 时不加沙箱。
- **Intent**：`exec::analyze` 后填 `known_readonly`。只读判断只是给核心的依据，核心自动放行只读命令时仍然
  应该给 `read_only` 沙箱（exec 设计文档里 `.git/config` 的问题）。
- 给模型的文本：`strip_ansi` → `to_valid_utf8` → `truncate_middle(max_result_bytes)`，**截断之后**再追加
  状态行：退出码非 0 时 `[退出码 N]`，超时 `[超时，已在 Ns 后终止]`，信号 `[被信号 N 终止]`，取消
  `[已被用户中断]`；没有输出时写 `（无输出）`。退出码 0 不写状态行。
- 在沙箱里失败、输出里有 `Permission denied`、`Read-only file system`、`Operation not permitted`、
  `Could not resolve host` 这类字样时，追加一行「命令在沙箱中执行：不能写工作区外、不能联网」，让模型知道
  换个做法或向用户说明，而不是反复重试。
- `on_output` 把原始输出块交给核心，核心 `post` 给界面。exec 已经设了 `NO_COLOR`、`TERM=dumb`，一般没有
  ANSI；有的话由界面处理。
- 后台常驻进程（`server &`）会在主进程退出后被清理，说明里写明不支持。
- display：`BashView`；`output` 较大，会话里会转成 blob。

### grep

参数：`pattern`，`path`（默认根目录），`glob`，`type`，`ignore_case`，`context`，`files_only`。

- 调 `workspace::grep`，`max_matches = grep_max_matches`。
- 输出沿用 rg 默认格式，模型最熟悉：匹配行 `path:line:text`，上下文行 `path-line-text`；`files_only` 时
  每行一个路径。截断时补一行 `[结果已截断，请缩小范围]`。
- 正则错误（`bad_pattern`）→ `is_error`，带 rg 的报错原文。
- display：`GrepView`（`files_only` 时 `lines` 里只有 `path`）。

### glob

参数：`pattern`，`path`（默认根目录）。

- 调 `workspace::files`，`sort_by_mtime = true`，`max_files = glob_max_files`。每行一个路径。
- rg 的 `--glob` 是 **gitignore 语义**，不是 shell glob：`*.cpp` 匹配任意深度。说明里要写清楚，并给出
  `src/**/*.hpp` 这样的例子。被 `.gitignore` 忽略的文件和隐藏文件不列出。
- display：`GlobView`。

### MCP 工具

`add_mcp` 把 `mcp::Client::tools()` 里的每一项包成一个 Tool：名字用 `qualified_name`，Schema 用
`input_schema`，Intent 为 `external`。

- 结果转文本：`text` 块直接拼接；`image`/`audio` 块写成 `[图片 image/png，N 字节，未展示]`；`resource` 块
  写 uri，带文本就附上；没有 `content` 只有 `structured` 时输出紧凑 JSON。`is_error` 沿用 `isError`。
- `McpError` 除 `cancelled` 外都转成 `is_error` 的结果；`disconnected` 时置 `McpView::disconnected`，
  核心据此决定是否重新 connect。
- `refresh_tools` 之后核心调用 `remove_prefix("mcp__<server>__")` 再 `add_mcp`。
- display：`McpView`。

## 实现要点

- **参数校验**：`arguments` 可能是坏 JSON，返回 `is_error`，带 nlohmann 的报错位置。必填字段缺失、类型错
  → `is_error`，指出字段名。**宽容一种常见错误**：整数和布尔字段收到字符串 `"10"`、`"true"` 时照常接受；
  未知字段忽略。
- **所有给模型的文本必须是合法 UTF-8**：nlohmann 在 `dump()` 遇到非法 UTF-8 时会抛 `type_error.316`，
  会让整条消息发不出去。rg 输出、bash 输出、MCP 文本都要经过 `to_valid_utf8`。
- **状态行放在截断之后**：先截断正文再追加说明，否则说明可能被 `truncate_middle` 切掉。
- **FileTracker 按 `resolve` 之后的路径做键**：`./a`、`a`、指向同一文件的符号链接都算同一个文件。
  内部加锁：只读调用可能被核心并行执行。
- **大文件的读取与编辑**：`workspace::read_text` 读到 `max_read_bytes` 就截断，现在是 64 KiB，read 翻不到
  后面、edit 改不了稍大的文件。见下面「对已完成模块的调整」。
- **Spec 的顺序要稳定**：DeepSeek 等网关按前缀缓存 prompt，工具列表顺序一变缓存就失效。
- 工具说明（`Spec::description`）直接写在 `.cpp` 里的原始字符串中，和 Schema 放在一起，改了一起审。

## 对已完成模块的调整

- `files.max_read_bytes` 的意思改成「read 能翻页的最大文件」，默认值提到 8 MiB；给模型的上限改用新的
  `tools.max_result_bytes`。
- `files.max_write_bytes` 默认值提到 1 MiB：edit 要先把整个文件读进来，128 KiB 对一般源文件偏小。
- app 增加 `tools` 段的映射，`config/dagent.json` 补上对应的默认值。

## 对核心的要求

这几条在工具层做不了，只能由核心保证，写在这里避免遗漏：

- **每个 tool_call 都要有一条 tool 消息**，包括未知工具、参数错误、用户拒绝、被取消的调用；否则 OpenAI
  协议下一次请求直接 400。顺序与 `tool_calls` 一致。
- 可以并行的只有 `Intent::Kind::read` 和 `known_readonly` 的 bash；write、edit、MCP 串行。
- FileTracker 跟着会话走；恢复会话时不重建，模型需要重新 read 才能编辑——宁可多读一次，也不能拿旧 Stamp
  覆盖别人的改动。
- read 可以读到 `.env` 这类文件，内容会发给模型。是否拦截由核心的权限策略决定（Intent 里有路径）。

## 本期不做

多处编辑（`edits` 数组）、后台 shell、web_fetch、todo、图片读取（编解码器还只支持文本）。等最小循环跑通、
看到真实的使用情况再加。

## 里程碑

| 里程碑 | 内容 |
| --- | --- |
| **T1 只读工具** | 接口、Registry、Context、read / grep / glob，对已完成模块的调整 |
| **T2 写入工具** | FileTracker、edit / write |
| **T3 bash** | bash、Intent 的只读分析、沙箱 |
| **T4 MCP** | `add_mcp` 与结果转换 |

## 验收（temp/tools_check）

直接调用工具，另外写一个只在 temp 里存在的最小循环，用 DeepSeek 做端到端检测（本地 qwen 网关会丢
`tools` 字段）。

**T1**

1. read 一个 3 MB 的日志文件，offset 指到接近末尾：行号正确，结尾提示正确；再读一个 CRLF + BOM 文件，
   输出里没有 `\r`、没有 BOM。
2. read 二进制文件、目录、不存在的路径（给出相近路径），三种结果各自符合描述。
3. 在本仓库 grep：正常结果、`files_only`、错误正则返回 rg 的报错、超出 `grep_max_matches` 时带截断提示。
4. glob `src/**/*.hpp` 按修改时间排序；`*.cpp` 能匹配到深层文件。
5. DeepSeek 端到端：「`truncate_middle` 在哪里定义、怎样处理 UTF-8 边界」，模型用 grep + read 回答正确。

**T2**

6. 没 read 就 edit 被拒；read 之后 edit 成功，CRLF 和 BOM 保留（按字节比较）。
7. read 之后在外部改文件，edit 返回 stale；重新 read 后成功。连续两次 edit 不需要重新 read。
8. `old_string` 出现 3 次：返回 3 个行号；`replace_all` 后 3 处都替换。带行号前缀、缩进不一致时提示正确。
9. 含非法 UTF-8 的文件拒绝 edit；write 新建多层目录下的文件；覆盖一个没读过的已有文件被拒。
10. DeepSeek 端到端：在 temp 里的小项目上修一个 bug，模型用 read + edit 完成，构建通过。

**T3**

11. `sleep 100` 配 1 秒超时得到超时状态行；运行中触发 stop，返回 `interrupted` 和已有的部分输出。
12. 输出 10 MB：给模型的文本在预算内且保留头尾，`on_output` 收到了全部字节。
13. `workspace_write` 沙箱里 `touch ~/x` 失败并带沙箱提示，`curl` 失败；`git status` 的 Intent 为只读，
    `rm -rf x` 不是。

**T4**

14. 连接 `server-everything`：工具以 `mcp__everything__*` 出现在 Registry；调用 echo 成功；图片块转成占位文本；
    `isError` 的结果带 `is_error`。

## 审核关注点

`run` 是否真的不抛异常；所有给模型的文本是否都过了 `to_valid_utf8`、状态行是否在截断之后；edit 的 stale
检查是否在 prepare 和 run 两处都做了；FileTracker 的键是否是 resolve 之后的路径。

## 已定

- edit 只做精确匹配，失败时给提示，不做模糊回退。
- bash 每次都是新进程，不保留 cwd。以后如果要保留，再考虑和并行执行的冲突。
- display 按工具定义结构体（`View`），不直接用 JSON。
