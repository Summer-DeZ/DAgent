# app：安装根、配置与命令行

头文件在 `src/public/app/`，实现在 `src/private/app/`，分成两个库和两个入口，其他模块都不依赖它：

| 目标 | 内容 | 依赖 |
| --- | --- | --- |
| `dagent_app_cli` | 命令行解析、安装根、后端启动器、run 输出适配、进程信号 | client、protocol、ipc、base；CLI11 私有 |
| `dagent_app_config` | 托管工具准备、搜索服务生命周期、配置/资源读取、提示词渲染、会话与查询装配 | runtime、llm、tools、storage、workspace、exec、mcp、base、curl、OpenSSL Crypto |
| `dagent`（`app/main.cpp`） | 前端可执行文件 | `dagent_app_cli` + `dagent_ui` |
| `dagent-backend`（`app/backend_main.cpp`） | 后端可执行文件，只接受 `--ipc-fd` | `dagent_backend` + `dagent_app_config` |

前端不链接执行、SQLite、MCP 或 HTTP 实现；后端不链接 UI/TUI。两者的通信见 [protocol](protocol.md)。

`app/bootstrap.hpp` 的装配入口实现 `backend/assembly.hpp` 中的 `backend::Assembler`，
返回静态持有 backend 配置端口的 `backend::Assembled`。
app 同时实现 `backend/configuration.hpp`、`backend/queries.hpp`、`backend/login.hpp` 中的端口。
这些契约仅含类型与内联定义，现有 `src/public` include 路径已覆盖，`dagent_app_config` 的链接依赖不变，
无需链接 `dagent_backend`；backend 不依赖 app，runtime 也不反向依赖 backend。

## 1. 自包含安装目录

运行时只认一个安装根：

1. `DAGENT_HOME` 非空时使用它；路径绝对化后必须已经存在且是目录。
2. dev preset 构建默认使用源码树的 `home/`。
3. 其它构建读取 `/proc/self/exe`，使用真实可执行文件所在目录。符号链接入口不会改变安装根。

启动会在根目录创建并删除一个 0600 临时文件以确认可写。失败以配置类错误退出，并提示通过 `DAGENT_HOME`
选择可写目录；不会回落到 HOME 或 XDG 目录。

安装根选择和目录验证在 `app/paths.cpp`；固定缓冲区 readlink 与可写探针位于 `app/paths_posix.cpp`，
经 `detail::executable_home_directory`、`detail::probe_home_writable` 调用。
`DAGENT_HOME` 经 `base::environment_variable` 读取，缺失与空值均使用后续默认值；环境值和 dev 默认文本
经 `base::path_from_utf8` 转为路径，再沿用原规范化与绝对化回退，目录错误中的路径经 `path_to_utf8` 输出。

```text
<root>/
├── dagent
├── dagent-backend     前端从同目录启动
├── libexec/           SearXNG 启动入口与按安装路径生成的 AppArmor profile（仅 Linux）
├── config/            config.json、models.json、mcp.json、runtime.json
├── prompts/           system.md、compact.md
├── AGENTS.md          可选的全局用户指令
├── themes/
├── agents/            子 Agent 定义
├── skills/            全局 Skill
├── memory/            全局与项目记忆，首次保存时创建目录
├── data/dagent.db     首次写入会话时创建
├── data/credentials/  登录凭据（0700；各 JSON 0600）
├── run/               runtime、模型与会话写锁，credentials/ 下固定登录锁，搜索实例运行配置
├── runtime/           托管工具与消费者依赖环境
├── cache/             opencode-go.json 公共模型清单；packages/ 下载与包缓存
└── logs/              dagent-<pid>.log
```

数据库和日志路径不可配置。日志带 pid（前后端各自一个文件），因此同一安装根的多个进程不会争用 rotating sink。
前端解析安装根后在 `app.initialize` 里交给后端；后端不自行决定安装根。

源码树中这些可管理文件统一放在 `home/`；dev 可执行文件无需设置环境变量就会读取这个目录。显式
`DAGENT_HOME` 仍然具有最高优先级。安装只补充缺失的用户资源；二进制、libexec 中的 SearXNG 入口与生成的 profile 随版本更新。
`libexec/` 始终相对后端二进制定位，不随 `DAGENT_HOME` 迁移。目录职责与首次配置见 [home](home.md)，工具环境见 [toolchain](toolchain.md)。

## 2. 配置与提示词

配置对外接口仍集中在 `app/config.hpp`，实现按变化原因拆在 `src/private/app/`：

| 文件 | 职责 |
| --- | --- |
| `config.cpp` | config.json 主流程、选项映射、覆写与未知键告警、MCP 配置解析及工具环境装配 |
| `models.cpp` | models.json 加载、生成/决策模型映射、模型添加 |
| `models_posix.cpp` | 模型与外观共用的 ModelWriteLock 原生实现 |
| `subagents.cpp` | 子 Agent 文件加载、原有 frontmatter 字段解析与行号诊断 |
| `appearance_config.cpp` | 外观与布局解析、外观配置保存 |
| `config_document.cpp` | `detail::ConfigDocument`：保留原文格式的 JSON 文档编辑，供模型添加和外观保存共用 |
| `config_detail.cpp` | 跨文件共用的配置读取、错误、路径、数值及环境变量辅助 |
| `config_detail_{posix,win}.cpp` | require_private_file 的普通文件与平台私有权限检查（POSIX 0600，Windows 私有 DACL） |

内部共享声明位于 `src/public/app/config_detail.hpp` 的 `dagent::app::detail` 命名空间。
配置环境展开的 `env_value` 通过 `base::environment_variable` 读取独立副本，缺失或空字符串仍抛
`ConfigError::Kind::invalid`，保留 `references unset environment variable` 错误文本与调用位置。
模型添加与外观保存继续使用同一个 `ModelWriteLock` 和 `run/models.lock`；各入口的锁范围不变。
ModelWriteLock 通过 pimpl 隐藏原生状态，构造/析构签名不变；Impl 在计算锁路径和创建目录前分配。
仍以 `O_RDWR | O_CREAT | O_CLOEXEC`、0600 打开锁文件，阻塞 `LOCK_EX` 仅重试 EINTR；
其他加锁错误先保存错误文本、close、置 -1，再抛原 ConfigError。析构显式 `LOCK_UN` 后 close。
POSIX require_private_file 仍使用 stat，保留原检查顺序和错误文本，不增加 symlink 或 uid 检查。
Windows require_private_file 拒绝目录与 reparse point，并验证当前用户/SYSTEM/Administrators 的受保护私有 DACL；
不安全时给出 Security > Advanced 的所有者、继承和授权修复说明，不提示 chmod。
Windows ModelWriteLock 使用同步 LockFileEx 阻塞独占固定字节范围，析构解锁并关闭；模型保存继续使用 workspace 的权限保留。

这两个平台源文件均由 `DAGENT_PLATFORM` 在 CMake 中选择。

`config/config.json` 由用户维护，包含权限、UI、HTTP、网页搜索/抓取、记忆、上下文、工具、进程和日志策略；`config/models.json` 保存模型
条目和默认模型。加载不做分层合并，不读项目配置、`.mcp.json`、`.env` 或 XDG 路径，也没有信任子系统。
`prompts.system` 和 `prompts.compact` 分别指定主提示词与上下文压缩提示词；相对路径以安装根为准，默认是
`prompts/system.md` 与 `prompts/compact.md`。两份提示词都由后端启动装配时读取并保存在快照中，不编入二进制；修改后重启后端生效，无需重新构建。
只有后端读取 config.json、models.json、提示词与凭据；前端只读取初始化结果给出的 UI 主题文件。

`models.json`：

```json
{
  "default": "local",
  "models": {
    "local": {
      "kind": "openai-chat",
      "base_url": "http://127.0.0.1:10010/v1",
      "model": "Qwen3.6-35B-A3B",
      "context_window": 131072,
      "max_tokens": 8192
    },
    "opencode-go/your-opencode-model-id": {
      "kind": "openai-chat",
      "base_url": "https://opencode.ai/inference/go/openai/v1",
      "model": "your-opencode-model-id",
      "login": "opencode",
      "max_tokens": 8192
    }
  }
}
```

模型的 `api_key` 可以直接保存，也可以写成 `env:VARIABLE_NAME`。启动时只从进程环境解析后一种形式。任何模型
列表只显示认证来源（key、login:opencode 或空），不输出值。`models.json` 不是严格 0600 时拒绝启动并给出 `chmod 600` 提示。

每个模型可选 `login: "opencode"`，与 api_key 按字段存在性互斥（同时出现时 null/空串也报错）。
加载只检查提供方、协议和允许的 base_url，不读凭据。OpenCode 默认头自动包含 `x-opencode-session: {session_id}`。
未登录不影响其它模型。`login` 专用启动模式跳过模型与工具配置，支持配置损坏时重新登录或登出。

`models` 与 `decision_models` 可选字符串映射 `headers`，显式 null、非字符串值、大小写重复键均报错。
`${ENV}` 在加载时展开（变量未设置或为空报错）；`{session_id}` 在每次请求时替换，其它花括号原样保留。
主回合与压缩用所属会话，父审阅用父会话，子 Agent 用自己的会话，d3 用被审阅动作的 ApprovalIdentity。
缺少所需会话 ID 报错。合并顺序为协议内置、提供方默认、条目头，按 ASCII 大小写不敏感覆盖。
头名须为合法 token，值不能含控制字符；传输头、Content-Type、Accept、User-Agent 与托管认证头保留，
完整名单见 [llm](llm.md#1-接口与边界)。decision_models 禁止 login。
头值只留在内部配置，不进入 UI、公开模型信息或会话记录。

交互界面的 `/model` 面板可按 `a` 添加模型。表单收集 kind、配置名、base URL、模型 ID、密钥、输出上限和
上下文窗口；支持 `openai-chat`、`anthropic`、`ollama`。后端在落盘前用与启动相同的解析器校验全部模型，拒绝
重名或缺失必填字段，然后通过同目录临时文件、fsync、rename 原子更新 `models.json`，保留其 0600 权限。
整个「重读 → 校验 → 写入」由安装根 `run/models.lock` 的短期 flock 包围，两个前端同时添加模型不会丢失另一项。
添加成功后当前会话立即切换到新模型，`default` 不自动改变。`headers`、`temperature`、`extra_body` 等高级字段仍可直接编辑 JSON。

`config/mcp.json` 独立保存 `mcpServers`、`connect_timeout_ms` 和 `probe_timeout_ms`，要求 0600。
stdio 项支持 `command`、`args`、`env`、`environment`，HTTP 项支持 `url`、`headers`；`${VAR}` 从进程环境展开。
两者都必须携带显式 `permissions` profile；stdio 在 MXC 沙箱内按范围启动，HTTP 要求 endpoint 网络目标获准。
`config/config.json` 不再允许重复声明 `mcp`。stdio 默认使用托管环境，可指定 `mcp/<name>` 或显式 `project`。

提示词、主题等配置路径相对于安装根；`--set key=value` 中的对应路径相对于 cwd。沙箱额外路径始终按下文的工作目录规则解析。未知键记 warning，类型和值错误
抛 `ConfigError`。`--set` 是一次性覆写，不写回配置；`--model` 命中名字时选择条目，否则临时覆写当前模型 ID。

`sandbox` 是独立版本化对象，当前只接受 `version: 3`、`backend: mxc`、`host_access: ask_once`；
旧版配置被拒绝并提示手改，不做静默迁移，也不切换到旧后端：`version` 不为 3 时报
`… must be 3: sandbox.version 2 was the removed SRT backend and 1 the removed Landlock backend; set version=3 and backend=mxc`，
`backend` 不为 `mxc` 时报 `… must be mxc: the SRT backend was removed; set version=3 and backend=mxc`。
`extra_readable` / `extra_writable` 是宿主维护的持久范围，
相对路径按 workspace cwd 解析，不改变实际沙箱能力，也不覆盖控制数据、敏感读取或 `.git` 保护。
`allowed_targets/denied_targets` 表达网络目标；`network_approval_timeout_ms`（默认 120000）与
`max_network_requests_per_execution`（默认 32）是运行中网络审批的等待上限与每次执行的提问预算，必须为正。
`startup_timeout_ms` 仍被解析并要求为正，但当前没有任何代码读取它（旧 SRT 的 bridge 启动握手已随后端移除）。

主要运行策略由 `home/config/config.json` 显式提供，缺少必填项时报配置错误。
`run.max_total_tokens` 缺省为 0；`subagents.approval`、`web` 和 `memory` 缺省采用下表值，兼容未添加这些字段的配置。

| 配置位置 | 用途 | 随附值 |
|---|---|---|
| `run.max_model_calls` | 每轮模型调用上限，0 不限 | 100 |
| `run.max_tool_calls` | 每轮工具调用上限，0 不限 | 200 |
| `run.max_total_tokens` | 本轮模型输入与输出总预算，0 不限；普通请求、父审阅、自动摘要及重试共享 | 0 |
| `run.max_model_retries` | 模型请求重试次数 | 2 |
| `run.max_parallel_tasks` | 子 Agent 并发数，必须为正整数 | 4 |
| `run.max_parallel_tools` | 只读工具并发数，必须为正整数 | 8 |
| `subagents.approval.mode` | `parent_when_unrestricted` 或 `user`，决定子请求是否可交父模型审阅 | parent_when_unrestricted |
| `subagents.approval.max_reviews_per_turn` | 每父 turn 审阅次数上限，0 表示不允许审阅 | 16 |
| `subagents.approval.review_timeout_ms` | 从请求创建起计时，包含排队、模型调用和重试；必须为正 | 60000 |
| `subagents.approval.failure` | 仅支持拒绝并反馈，不自动转人工 | deny_with_feedback |
| `memory.enabled` | 向主 Agent 提供 memory 工具与 system 索引，不控制用户管理面板 | true |
| `memory.max_index_bytes` | 索引文本预算（含分组标题），正整数 | 8192 |
| `memory.max_entry_bytes` | 含 frontmatter 的完整条目读写上限，正整数 | 4096 |
| `web.timeout_ms` | 单次网页请求（含全部重定向跳）的上限，含沙箱启动与等待审批；正整数 | 120000 |
| `web.startup_timeout_ms` | SearXNG 启动等待上限；正整数 | 30000 |
| `web.max_body_bytes` | HTTP 正文下载上限，1–67108864 | 2097152 |
| `web.cache_bytes` | 每个 Context 的页面缓存预算，至少等于下载上限 | 16777216 |
| `web.engines` | 非空的 SearXNG 引擎名称列表；配置中没有端口字段 | yahoo / brave / duckduckgo |
| `tools.bash_collect_bytes` | 中断时命令输出收集上限 | 4194304 |
| `ui.theme_file` | 兼容的主题文件路径；appearance.theme 缺省时使用 | themes/dagent.json |
| `ui.appearance.theme` | dagent 或主题文件路径（相对 Home 或绝对路径） | dagent（优先兼容 theme_file） |
| `ui.appearance.mode` | auto / dark / light | auto |
| `ui.appearance.background` | solid / transparent / #rrggbb | solid |
| `ui.appearance.accent` | orange / green / violet / blue / #rrggbb | orange |
| `ui.appearance.panels` | filled / outline | filled |
| `ui.appearance.density` | comfortable / compact | comfortable |
| `ui.appearance.icons` | unicode / nerd / ascii | unicode |
| `ui.layout.width` | auto / compact（88）/ wide（最多 160）/ 整数 60–240 | auto（88–120） |
| `ui.layout.status_panel` | right / left / hidden | right |
| `ui.layout.navigator` | auto（终端 ≥170 列）/ always / hidden | auto |
| `ui.layout.navigator_sections` | 导航栏有序分节 id | sessions, git, skills, agents |
| `ui.layout.status_sections` | 状态栏有序分节 id | session, context, plan, changed, running_agents, mcp |
| `ui.appearance.diff` | auto / unified | auto |
| `ui.appearance.links` | on / off | on |
| `ui.completion_max_files` | 每次文件补全扫描上限；每次查询读取当前目录 | 5000 |
| `session.history_scan_limit` | 每页历史最多扫描的记录数 | 100 |

`ui.appearance` 整体及各键均可省略；显式提供时必须是对象和合法字符串，错误消息指明
`/ui/appearance/<key>`。无效枚举、颜色、空主题名或不存在的主题文件为配置错误。
ConfigurationGateway 向前端返回解析后的有效值，保存入口更新 ui.appearance 和可选 ui.layout，保留其它配置。
midnight / paper 预设已删除，配置错误提示改用 dagent 或主题文件路径；设置对话框只保留 Mode，不再提供 Theme 行。
强调色预设按名字保存，按 dark/light 分别解析为 orange `#fe8a4c` / `#a85500`、
green `#87d787` / `#2a7a1a`、violet `#b9a0ff` / `#6a4bc4`、blue `#7a9cff` / `#3157c8`。
加载时将旧 hex `#EC5B2B` / `#7fd17f` / `#c792ea` / `#6ba1e6`（不区分大小写）映射为对应名称，
下次保存写名字；其它 hex 保留输入值，渲染时按 OKLab 明度在浅色压暗、深色提亮至面板上 4.5:1。
`/settings` 的保存立即生效且跨重启保留；Esc/Ctrl+C 只撤销前端预览。预设主题都随 Home 资源安装。

`ui.layout` 整体与所有键均可省略，显式值须为合法对象、枚举、整数或有序字符串数组；错误定位 `/ui/layout/<key>`。
全部合法分节为 sessions、git、skills、agents、session、context、plan、changed、running_agents、mcp，
可放入任一栏，两栏合计至多出现一次；未知或重复 id 报配置错误，不在两列表中的分节隐藏。
正文宽度会按实际主区收窄，通常每侧留至少四列；双栏 diff 保留独立突破规则。
旧 `ui.appearance.sidebar` 仍校验并在新 `status_panel` 缺省时映射 right / left / hidden；两者同时给出时以新键为准。
`/settings` 的 Layout 页面提供布局选项和三列分节移动/排序，保存时写新键并移除 sidebar；Ctrl+B / Ctrl+T 的临时开关不落盘。

配置的读改写使用 `ordered_json`，按原顺序更新已有键、新键追加至所属对象末尾，并将变更值补入原文。
未修改字段的注释、空白、行内数组和数值文本不变；新内容沿用文件缩进、LF/CRLF 与末尾换行风格。
修改原有行内数组时仍保持一行，元素间使用逗号加空格。保存外观和布局时，缺失且等于默认值的键不补写；
已有键即使改回默认值也保留并更新，旧强调色 hex 的名称迁移仍生效。
`ui.appearance.save`（含可选 layout）、`model.add` 与 `provider.model.add` 共用这一保序保存路径，沿用锁与原子写。

QueryGateway 提供 `workspace.git_status` 与 `agents.list`。Git 查询使用与 workspace.info 相同的只读受管 git 环境，
无网络、每条命令 2 秒、每路输出最多 1 MiB、结果最多 2048 个文件；numstat 合计暂存和未暂存改动，二进制计数为 0。
交互模式由后端应用层 inotify 监听至多 4096 个工作区目录，跳过受保护路径和 git objects；事件只触发前端防抖查询。
无可用监听时仍保留轮次、工具完成、终端重新聚焦和打开导航的刷新；退出先停止监听，再关闭 Publisher。
`queries_posix.cpp` 实现 `watch_workspace`，`queries.cpp` 的共享私有成员 `should_watch_directory` 保留目录保护条件。
根目录仍先直接加入监听，遍历时先检查保护条件，再检查目录且非符号链接；保留 `.git`、`.git/refs`、`.git/logs`
的原例外以及 `.git/objects` 排除。取消通过 eventfd 唤醒，poll 失败仍退出；重扫先于 changed 回调。
线程内取消回调销毁后才关闭 inotify/eventfd，重新设置监听仍先停止并等待旧 jthread。
Agent 清单使用启动时加载的定义，不返回 system prompt。

上下文预算、工具结果与文件大小限制、HTTP/进程超时、日志大小及进度间隔也显式读取对应配置段。
`process.kill_grace_ms`、`process.drain_after_exit_ms`、`process.env_deny` 与 `log.also_stderr` 已列入配置文件。
HTTP 和进程配置同样传入 MCP；模型请求使用配置的总超时与空闲超时，不再强制覆盖。
web 工具的传输超时和正文预算来自独立的 `web` 段，输出上限仍由 `tools.max_result_bytes` 控制。
启动时解析宿主 curl 路径和 `internal/searxng` 环境目录；引擎名交由 SearXNG 启动时加载，详见 [web](web.md)。
子 Agent 在 `home/agents/*.md` 的 frontmatter 中设置 `max_model_calls`、`max_tool_calls`，省略或为 0 时继承全局配置。
模型输出预算统一设置在 `models.json` 每个模型的 `max_tokens`，必须大于 0；禁止通过 `extra_body` 再提供输出预算。
Chat 编码为 `max_completion_tokens`，Anthropic 编码为 `max_tokens`，Ollama 编码为 `options.num_predict`。

### 决策审阅配置

`config/models.json` 增加决策目录与选择（不影响生成模型的 `default`、`models` 与 `--model` 语义）：

```json
{
  "decision": null,
  "decision_models": {
    "d3-local": {"kind": "systemone", "endpoint": "http://127.0.0.1:10006/v1/systemone",
                 "model": "d3", "model_revision": "deployment-1"}
  }
}
```

`config/config.json` 的行为配置：

```json
{"decision_review": {"timeout_ms": 10000, "threshold_profile": "risk-first-v2-fixed-20261018", "threshold_file": "config/decision/risk-first-v2-fixed-20261018.json"}}
```

`decision` 缺省或 null 表示禁用；命名不存在的条目或不支持的 kind 报配置错误。阈值数据在
`data/decision.sqlite`，`threshold_profile` 用于正式与临时配置分开；阈值按 endpoint、协议 kind、
适配器版本、model、model_revision、问句版本与 profile 隔离。
当前风险优先问句版本是 `risk-first-v2`；升级不会自动复制旧版阈值，也不会自动启用决策模型。
没有与当前身份匹配的完整场景配置时仍进入后续审批。正式或试验参数应以独立 profile 名导入，
避免覆盖旧实验记录所使用的配置。

`app/decision_profile.*` 读取可选的 `decision_review.threshold_file`，将声明的完整版本与当前模型身份
逐项匹配，解析显式场景参数并交由存储层校验及幂等导入。没有文件配置时保留只读取数据库的行为。
文件路径在 config.json 中相对 Home，在 `--set` 中相对工作目录；模板文件随 Home 安装，不覆盖用户已有配置。
文件读取、版本匹配或存储导入失败时保留“已配置决策模型”的事实，使强制复审与网页扫描继续按失败回退。

## 3. 工作目录与项目根

`Args::cwd` 是 `-C` 指定目录或进程启动目录，工具路径、沙箱和会话列表都以它为准。`project_root(cwd)` 仍用于
收集 git 与 AGENTS.md 上下文：能执行 `git rev-parse --show-toplevel` 时取 git 根，否则就是 cwd。进程不 chdir。

会话归属只比较规范化后的精确 cwd，不再用 git 根合并不同子目录。

## 4. 命令行

```
dagent [选项] [提示词…]
dagent run [选项] <提示词…>
dagent sessions
dagent --list-models
dagent login [opencode]
dagent logout opencode
dagent runtime sync
dagent runtime list
dagent sandbox status

通用：-C/--cwd  -m/--model  --set  -r/--resume  --continue  --log-level
      --permissions <ask|workspace|unrestricted>  --read-only  --plan
run： --output <text|json|jsonl>
```

`queries.cpp` 的 Git 状态查询通过 `exec::system_git_program` 取得 POSIX 固定路径 `/usr/bin/git`，
不预先验证或搜索 PATH，仍沿用只读 MXC 沙箱、命令参数和输出处理。

`sandbox_status.cpp` 保留 runtime 装配、静态检查与真实 probe（`sandbox_runtime`、`sandbox_probe`、`sandbox_status`，
探测语义见 [exec](exec.md#装配能力快照与探测)）；`sandbox_status_{posix,win}.cpp` 提供 `detail::platform_sandbox_supported`、
`detail::sandbox_containment`、`detail::host_prerequisites` 与 `detail::append_host_diagnostics`。
`sandbox_runtime` 失败时返回原因字符串（runtime 未准备、MXC 库未安装、库加载或版本错误、托管 bash/rg 缺失、本平台没有后端），
装配层把它写入 `Support.missing`。

`dagent sandbox status` 输出一个 JSON 对象：

| 字段 | 内容 |
| --- | --- |
| `toolchain_current` | runtime 是否为当前配置准备好 |
| `static` | 依次为 `runtime`、`mxc-library`（库文件路径）、`mxc-load`（成功时为库版本，加载失败时为错误文本，未安装时为 `requires MXC 1.0.1`）、`bash`、`rg`，以及宿主前置 `bwrap`（路径与版本，要求 ≥ 0.5）、`slirp4netns`、`unshare`、`nsenter`、`setsid`、`ip`、`iptables-restore`；缺失项给出所需的包名 |
| `platform_support`、`available_backends` | 库加载成功时原样输出 `mxc_platform_support_json` 与 `mxc_available_backends_json`（如 `bubblewrapNetwork.proxyEnforcement`） |
| `probe` | 真实启动结果；必要条件缺失时为 `{ok:false, stage:"skipped", error:"missing: <项>"}`，不启动 |
| `host_prerequisite` | 探测失败且错误含 namespace、uid_map、uid map、loopback 或 unshare failed 时给出部署 `dagent.apparmor` 的提示 |
| `host` | 可读取时的 `apparmor_restrict_unprivileged_userns`，以及后端进程当前的 AppArmor 标签 `apparmor_profile` |

宿主程序查找先用 exec::which，再检查 `/usr/bin`、`/usr/sbin` 下的固定位置；bwrap 另以 `bwrap --version`（5 秒超时）取版本。
真实 probe 后才按上述关键词追加 namespace 前置提示，读取两处 AppArmor `/proc` 信息；
保留单行读取、空限制值不写字段、非空时直接 stoi 的行为。必要条件缺失时提前返回，不读取宿主诊断。
没有后端的平台只输出 `toolchain_current`、只含 `runtime` 一项的 `static` 与 `probe.stage="unsupported"`。
监听和宿主诊断源文件均由 `DAGENT_PLATFORM` 在 CMake 中选择。

- `--read-only` 与权限三档正交；`--plan` 进入只读规划模式。
- `run` 可从 stdin 追加提示词；交互模式要求 stdin 是终端。
  CLI 的终端判断使用 `base::stdin_is_terminal`，读取由 `app/cli_posix.cpp` 的 `detail::read_stdin_all` 完成。
- `--continue` 选择当前 cwd 最近的会话，`--resume` 只接受当前 cwd 内的完整 ID 或唯一前缀。
- 退出码：0 成功，1 运行失败，2 参数/配置错误，130 中断。
- `run` 的 text/json 在最终写入并 flush 后检查 stdout；写入或刷新失败返回 1，即使模型已正常完成。

- `sessions` 与 `--list-models` 也经独占后端查询（查询模式不创建会话、不启动 MCP、不收集 git 环境）；只有 `--help` / `--version`
  和参数错误不启动后端。

`dagent login opencode` 显示授权 URL、用户码与 Go 订阅链接，以 argv 启动 xdg-open（macOS 为 open），
等待批准，Ctrl+C 取消；成功显示 workspace 和到期时间。`dagent login` 只查本地状态，不联网、不刷新。
`dagent logout opencode` 在共享锁内删除凭据；--list-models 的 AUTH 列为 key、login:opencode 或空。

`app/login.cpp` 实现 `backend/login.hpp` 中的 LoginGateway，由后端装配注入。models.dev/api.json 的 opencode-go
清单缓存在 Home/cache/opencode-go.json；打开时超过 24 小时刷新，refresh=true 强制刷新，离线使用缓存。
列表以 models.json 的实际条目计算 added；过滤允许地址之外的模型，Anthropic 项标为不可用。
新增模型 ID 为 opencode-go/<model>，由接口类型生成 kind/base_url，context_window 取 context，
max_tokens=min(output,32768)，send_reasoning_content 取 reasoning。沿用 add_model 的 models.lock、重读校验与原子保存，
已存在项不覆盖并直接切换，default 不变。TUI 双栏选择器、登录框与 Go 清单留待 3b。

## 5. 装配

前端解析 CLI 与安装根后启动后端，发送 `app.initialize{root, cwd, mode, ordered_overrides, permissions, read_only, plan,
resume_id, continue_last, log_level}`。后端的 `app::assemble_backend` 依次：

1. 维护命令执行 runtime sync/list 或 sandbox status，不创建会话。普通启动读取 config/ 下配置及 runtime 快照，按 argv 顺序应用 `--set` 与 `--model`；无效时返回 `config_error`，前端以 2 退出。
2. 初始化日志：`<root>/logs/dagent-<pid>.log`；交互模式关闭 stderr sink，run 与查询模式沿用配置。
3. 查询模式只构造配置网关与只读查询；interactive/run 模式另外收集一次工作区环境（git、AGENTS.md）、探测沙箱、
   读取子 Agent 定义与提示词，创建共享 `McpHub` 和懒启动的 `Searxng` 管理器；环境事实和子定义存入 `SessionAssembly::Options`。
搜索端点通过 `tools::Options::search_endpoint` 回调注入会话，每个后端共享一个搜索服务，每个 Context 独立缓存页面。
`HubResources` 与会话实例只持有共享 MCP Hub，不依赖装配数据包装。
模型解析与客户端工厂由 Configuration/启动装配负责；`load_models` 只读取模型文件并按序应用模型覆写，切模型不重复加载工具环境、MCP、Git 与子定义。Configuration 仅保存模型目录与主题路径，不保存完整 Config。

4. `SessionAssembly` 实现 `runtime::SessionFactory`：为每个会话渲染 system prompt、建立 `tools::Context` / Registry、
   打开记录写入器并取得写租约；存储路径固定为 `<root>/data/dagent.db`。

权限初值优先用 `--permissions`，再用 config.json，默认 workspace。凭据只在 app 内部的 `llm::ProviderConfig` 中出现，
对外只返回 `PublicModel`。CMake 安装两个可执行文件和分目录资源，模型与 MCP 由不含私有凭据的模板首次生成，
Linux 安装时设置权限为 0600；`libexec` 中的 SearXNG 入口和 AppArmor profile 也仅在 Linux 生成或安装。

## 6. run 输出

run 模式的前端由 `RunOutput` 累积主会话结果，并按所选格式输出：

| `--output` | stdout | stderr |
| --- | --- | --- |
| `text` | 结束时打印最后一步正文，不流式写入 | 工具进度、Notice、重试及等待模型的心跳 |
| `json` | 结束时一个结果对象 | 同 text |
| `jsonl` | 协议事件信封，一行一个 JSON | 启动错误及 warn / error Notice；info 留在 jsonl |

json 结果字段为 `session_id, status, error, result, steps, tool_calls, usage, duration_ms`。
jsonl 每行直接序列化 `protocol::Event`：`seq, session_id, session_generation, kind, data,
parent_session_id, model_call_id, agent`。子事件身份保留在同一信封中；
会话和操作通知也原样输出，恢复时不输出历史。
`memory` 沿用工具进度输出；jsonl 的 `tool_finished.data.view` 保留完整 MemoryView，text/json 的 stderr 显示操作摘要。
各格式共用主会话结果累积，在新步和流重试时重置正文缓冲，并从主会话 `turn_ended` 取得状态与用量。
子 Agent 事件不修改主会话结果，也不结束本次 run；JSONL 同样按最终状态返回退出码。

stdout 被关闭时 jsonl 请求取消本轮并返回 1，text / json 最终写出失败不改变运行状态。run 模式没有人工审批器：
主会话需要人工批准的调用返回「no interactive approver」工具错误，ask / exit_plan 返回原非交互文本。
符合 `subagents.approval` 与父权限条件的子请求仍可由父模型审批；没有可用路由时返回 approval unavailable，详见 [权限指南](../guide/permissions.md)。

### 信号与退出码

Linux 前端忽略 SIGPIPE，并在其他线程启动前屏蔽 SIGINT / SIGTERM，由 sigwait 线程处理。可优雅结束期间第一个信号经
`session.snapshot` + `run.cancel` 只取消本前端的当前 Run，第二个信号直接 `_Exit(130)`；加载配置、读取 stdin 等轮外阶段直接退出。
后端在独立进程组中运行，不接收终端信号。交互模式 raw 下的 Ctrl+C 是按键，行为由 ui 决定；全屏期间的进程信号走退出流程。

前端的 `install_interrupts`、`ignore_broken_pipe` 位于 `app/interrupts_posix.cpp`，main 按此顺序调用。
后端的 `initialize_backend_signals` 位于 `app/backend_main_posix.cpp`，仍在参数校验后、Channel 接管前调用；
参数遍历与 `--ipc-fd` 保持在共享入口中，值经 `InheritedEndpoint::parse_argument` 按 atoi 语义解析，
重复参数最后一次生效；校验后先初始化信号，再 `Channel::adopt`，最后 `prevent_inheritance`。

Windows 的对应实现位于 `interrupts_win.cpp`、`backend_main_win.cpp`。前端控制台回调处理 Ctrl-C/Ctrl-Break，
首次优雅停止只计数并通知事件，由工作线程调用 request_stop；轮外或第二次中断直接 TerminateProcess(130)，
不等待第一次 stop 回调完成。后端忽略这两类控制台事件，按 IPC 取消和生命周期 Job 收尾。
Windows 的 `--ipc-fd` 严格解析为十进制 uintptr_t 句柄，拒绝溢出、尾随文本和无效值；采用及禁继承顺序不变。

| 情况 | 退出码 |
| --- | --- |
| run 的 done；正常关闭交互；列表成功 | 0 |
| run 的 failed / limit / denied，启动或通信失败，jsonl 写出失败 | 1 |
| 参数或配置错误（包括后端返回 `config_error`） | 2 |
| run 被信号中断，或交互因进程中断信号退出 | 130 |

## 7. 全局技能

后端在启动时发现 `<root>/skills/*/SKILL.md`，把不可变目录共享给会话与查询网关。
格式、诊断和重启生效规则见 [skills](skills.md)。

## 8. Agent 记忆

后端通过 `HomePaths` 和同一 `project_root` 装配 `tools::MemoryStore`。`SessionAssembly` 在创建与恢复时
扫描并追加 `# Memory`，工具注册同时受 `memory.enabled` 与子会话限制控制；system 在打开期间固定。
QueryGateway 提供 `memory.list/view/delete`，供 `/memory` 分组查看与删除，关闭模型记忆不禁止用户管理已有文件。
存储格式、优先级、索引预算和模型写入准则见 [memory](memory.md)。
