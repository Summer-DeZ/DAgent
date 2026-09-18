# app：配置与命令行（入口层）

头文件在 `src/public/app/`，实现在 `src/private/app/`，构建为静态库 `dagent_app`，命名空间 `dagent::app`。
**位于最顶层**：它认识所有模块的 Options，所以依赖全部外围模块，但没有任何模块反过来依赖它。

| 能力 | 头文件 | 主要接口 |
| --- | --- | --- |
| 配置加载、密钥、项目信任 | `app/config.hpp` | `load_config`、`load_secrets`、`project_root`、`is_trusted`、`trust_project`、`parse_mcp_servers` |
| 命令行解析 | `app/cli.hpp` | `parse_args` |

程序入口（可执行目标 `dagent` 的 `main`）还没有：拿到 `Args` 和 `Config` 之后怎样组装界面、agent 和各模块，
属于核心，见第 5 节列出的约定。

---

## 1. 目录与文件布局

```
工作区根   Args::cwd：-C 指定的目录，或者启动目录。写文件免确认、沙箱可写的范围都以它为准
项目根     project_root(cwd)：cwd 所在的 git 根，不在仓库里就是 cwd。项目级配置、信任、会话归属以它为单位

用户级     $XDG_CONFIG_HOME/dagent/（默认 ~/.config/dagent/）
  config.json            用户配置
  mcp.json               用户级 MCP server（格式同 .mcp.json）
  .env                   用户级密钥；权限必须是 0600 或更严，否则忽略并给出警告
  trusted_projects.json  受信任的项目根：{"trusted": ["/abs/root", …]}，由 trust_project 写入

项目级     <项目根>/
  .dagent/config.json    项目配置        ┐ 只在项目受信任时读取
  .mcp.json              项目 MCP server ┘
  .env.dev、.env         项目密钥（不受信任限制）
  AGENTS.md              项目指令（由 workspace 的 context 收集，不受信任限制）

数据与状态
  $XDG_DATA_HOME/dagent/sessions/   会话（session.directory 可改）
  $XDG_STATE_HOME/dagent/logs/      日志（log.file 可改）
```

工作区根和项目根通常相同；在仓库的子目录里启动时，工作区只是这个子目录，改子目录外的文件需要确认。

---

## 2. 配置加载（load_config）

### 分层与优先级（从低到高）

| 层 | 位置 | 说明 |
| --- | --- | --- |
| 1 内置默认 | 各 Options 结构的默认值 | 没有任何配置也能跑 |
| 2 用户级 | `$XDG_CONFIG_HOME/dagent/config.json` | |
| 3 项目级 | `<项目根>/.dagent/config.json` | **只在项目受信任时读取** |
| 4 显式文件 | `--config <文件>` | 指定之后替换第 2、3 层；用户主动指定，不受信任限制 |
| 5 命令行 | `--set 键=值`、`-m 模型` | 按命令行上出现的顺序叠加，后面的覆盖前面的 |

- 合并用 `json::merge_patch`（RFC 7386）：对象递归合并，数组整体替换，`null` 删除这个键。
- 允许 `//` 注释。
- **相对路径相对于它所在的配置文件**（`gateway.system_prompt_file`、`session.directory`、`log.file`、
  `search.rg_path`），在合并之前就解析成绝对路径；`--set` 里的相对路径相对 cwd。`search.rg_path` 不含 `/`
  时是命令名（在 PATH 里找），不做解析。
- **未知键只警告**（日志里「未知配置项 gateway.modle」），不报错。
- **类型错误**抛 `ConfigError{type}`，信息里带 JSON 指针，比如 `/http/timeout_seconds 应为整数`。
- `--set` 的值先按 JSON 解析（`true`、`3`、`[1,2]`），失败就当字符串。键按 `.` 分段，所以无法指定本身
  含 `.` 的键名（比如 `network.credentials` 下的主机名），这类键要写在配置文件里。

### 映射到各模块

| 配置段 | 映射到 |
| --- | --- |
| `gateway` | `Gateway`（`api_key_env` 指向的变量经 Secrets 取值，本地网关可以为空） |
| `http` | `net::HttpOptions` |
| `process` | `exec::Options` |
| `files`、`search` | `workspace::FileOptions`、`workspace::SearchOptions` |
| `session`、`log`、`mcp` | `session::Options`、`base::LogOptions`、`mcp::Options` |
| `context`、`run`、`progress`、`permissions` | 暂时定义在 app 里（`ContextOptions` 等）；核心有了自己的类型后改为映射到核心 |
| `network.credentials` | `Config::credentials`：主机名 → 密钥值 |

键名到字段逐项手写映射（单位不同，如 `timeout_seconds` → `std::chrono::seconds`）。`api_key` 和任何密钥值
都不会出现在日志和错误信息里。

---

## 3. 项目信任

项目级的 `.dagent/config.json` 和 `.mcp.json` 能改网关地址（把你的 API key 发到别处）、指定 `rg` 路径、
清空环境变量过滤、改权限模式、启动任意命令，所以**克隆来的仓库默认没有这些能力**：

- 信任以**项目根**为单位，按规范化路径精确匹配，不继承到子目录或父目录。
- 未受信任时这两个文件都不读，记一条 warn，并列在 `Config::untrusted_files` 里。入口看到它非空时，
  交互模式应当询问用户是否信任，确认后调用 `trust_project(config.project_root)` 并重新 `load_config`；
  `run` 模式只警告，按未受信任继续。
- 授予信任的两条路：交互模式下的询问，以及 `dagent trust [目录]` 子命令（给 run/CI 场景用）。
- 信任列表损坏时按空处理（记 warn）：宁可多问一次，也不能因为文件坏了就当成全都信任。
- 项目的 `.env`、`.env.dev` 和 AGENTS.md 不受信任限制。

---

## 4. 密钥与 MCP server

### 密钥（load_secrets）

同一个键以先读到的为准：**进程环境变量** → **用户级 `.env`** → 项目 `.env.dev` → 项目 `.env`。用户级排在
项目级前面，不可信仓库的 `.env` 盖不住用户自己的密钥。密钥只在内存里，不写进进程环境（见 base 的 `Secrets`）。

### MCP server（.mcp.json）

- 来源：用户级 `mcp.json`，加上受信任项目的 `.mcp.json`。**同名 server 整条替换**（项目级优先），不逐字段
  合并；项目级写 `"name": null` 可以去掉用户级的同名 server。
- 格式沿用 Claude Code 等客户端的写法：`command` + `args` 拼成 `ServerConfig::command`，`env` 注入子进程；
  `type` 为 `http` 时用 `url` 和 `headers`；`type` 为 `sse`（已弃用的传输）或无法识别时跳过并记 warn。
- `${VAR}` 用 Secrets 展开；**变量不存在时抛 `ConfigError{invalid}`**，指出 server 和字段，而不是替换成空串
  （否则会在很久以后变成难以理解的鉴权失败）。
- **server 名**：工具名是 `mcp__<server>__<tool>`，名字用 `mcp::sanitize_name` 清理。清理后互相重名
  （`my.fs` 和 `my_fs`）或含 `__` 的 server 名抛 `invalid`，信息里指出是哪两个。mcp 模块自己只能发现同一个
  server 内部的冲突。

---

## 5. 命令行（parse_args）

```
dagent [选项] [提示词…]             进入交互界面；给了提示词就把它作为第一条消息
dagent run [选项] <提示词…>         非交互：跑完一轮后退出
dagent sessions                     列出最近的会话
dagent trust [目录]                 信任目录所在的项目
dagent --version

通用选项：-C/--cwd  -c/--config  -m/--model  --set  -r/--resume  --continue  --log-level
run 专用：--permissions <auto|deny>  --output <text|json|jsonl>
```

- **没加引号的多个词拼成一句**提示词（`dagent run fix the bug` → `fix the bug`）。最多一个子命令：进入
  子命令之后，提示词里再出现 `run`、`sessions`、`trust` 也只是普通的词；但提示词写在子命令名**前面**
  （`dagent fix run tests`）时意图说不清，以退出码 2 退出并提示加引号。
- `-m`、`--set` 每次出现只取一个值，可以重复；两者混用时按命令行上的真实顺序记录。
- `-C` 在解析阶段转成绝对路径，**不 chdir**；`trust` 的目录参数同样放进 `Args::cwd`。
- **stdin**：`run` 模式下 stdin 不是终端时读完它，拼成「参数 + 空行 + stdin 内容」，支持
  `git diff | dagent run "审查这个改动"`；拼完仍为空时以退出码 2 退出。交互模式要求 stdin 是终端，否则提示
  使用 `dagent run` 并以退出码 2 退出。
- **退出码**：0 成功；1 运行失败；2 参数错误；130 被 Ctrl+C 中断。CLI11 自己的错误码是 100 以上，这里统一
  映射成 2。`--help`、`--version` 返回 0。

### 入口要遵守的约定

因为不 chdir，进程的当前目录不一定是工作区根，入口组装各模块时要显式传 `Args::cwd`：
`workspace::resolve` 的 root、`exec::Command::cwd`、沙箱的 `writable`（不能依赖它「为空时用当前目录」的
默认值）、`workspace::collect_environment` 的 cwd，以及 `session::Meta` 的 `cwd` / `git_root`。

---

## 6. 依赖与构建

- CLI11 v2.7.2（header-only），在 `cmake/deps.cmake` 里通过 FetchContent 引入，带 `FIND_PACKAGE_ARGS`。
- `--version` 的版本号取编译宏 `DAGENT_VERSION`（根 `CMakeLists.txt` 的 `PROJECT_VERSION`）。
- 在 CMake 之外链接的检测程序，写法可以参照 `temp/app_check/build.sh`；它把 `XDG_CONFIG_HOME` 指到
  临时目录，不会碰真实的用户配置和信任列表。
