# app：安装根、配置与命令行

头文件在 `src/public/app/`，实现在 `src/private/app/`，分成两个库和两个入口，其他模块都不依赖它：

| 目标 | 内容 | 依赖 |
| --- | --- | --- |
| `dagent_app_cli` | 命令行解析、安装根、后端启动器、run 输出适配、进程信号 | client、protocol、ipc、base；CLI11 私有 |
| `dagent_app_config` | 托管工具准备、搜索服务生命周期、配置/资源读取、提示词渲染、会话与查询装配 | runtime、llm、tools、storage、workspace、exec、mcp、base、curl、OpenSSL Crypto |
| `dagent`（`app/main.cpp`） | 前端可执行文件 | `dagent_app_cli` + `dagent_ui` |
| `dagent-backend`（`app/backend_main.cpp`） | 后端可执行文件，只接受 `--ipc-fd` | `dagent_backend` + `dagent_app_config` |

前端不链接执行、SQLite、MCP 或 HTTP 实现；后端不链接 UI/TUI。两者的通信见 [protocol](protocol.md)。

## 1. 自包含安装目录

运行时只认一个安装根：

1. `DAGENT_HOME` 非空时使用它；路径绝对化后必须已经存在且是目录。
2. dev preset 构建默认使用源码树的 `home/`。
3. 其它构建读取 `/proc/self/exe`，使用真实可执行文件所在目录。符号链接入口不会改变安装根。

启动会在根目录创建并删除一个 0600 临时文件以确认可写。失败以配置类错误退出，并提示通过 `DAGENT_HOME`
选择可写目录；不会回落到 HOME 或 XDG 目录。

```text
<root>/
├── dagent
├── dagent-backend     前端从同目录启动
├── libexec/           SRT bridge、SearXNG 启动入口与按安装路径生成的 AppArmor profile
├── config/            config.json、models.json、mcp.json、runtime.json
├── prompts/           system.md、compact.md
├── AGENTS.md          可选的全局用户指令
├── themes/
├── agents/            子 Agent 定义
├── skills/            全局 Skill
├── data/dagent.db     首次写入会话时创建
├── run/               runtime、模型与会话写锁，搜索实例的运行配置
├── runtime/           托管工具与消费者依赖环境
├── cache/packages/    下载与包缓存
└── logs/              dagent-<pid>.log
```

数据库和日志路径不可配置。日志带 pid（前后端各自一个文件），因此同一安装根的多个进程不会争用 rotating sink。
前端解析安装根后在 `app.initialize` 里交给后端；后端不自行决定安装根。

源码树中这些可管理文件统一放在 `home/`；dev 可执行文件无需设置环境变量就会读取这个目录。显式
`DAGENT_HOME` 仍然具有最高优先级。安装只补充缺失的用户资源；二进制、两个 libexec 启动入口与生成的 profile 随版本更新。
`libexec/` 始终相对后端二进制定位，不随 `DAGENT_HOME` 迁移。目录职责与首次配置见 [home](home.md)，工具环境见 [toolchain](toolchain.md)。

## 2. 配置与提示词

`config/config.json` 由用户维护，包含权限、UI、HTTP、网页搜索/抓取、上下文、工具、进程和日志策略；`config/models.json` 保存模型
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
    }
  }
}
```

模型的 `api_key` 可以直接保存，也可以写成 `env:VARIABLE_NAME`。启动时只从进程环境解析后一种形式。任何模型
列表只显示 key 的有无，不输出值。`models.json` 不是严格 0600 时拒绝启动并给出 `chmod 600` 提示。

交互界面的 `/model` 面板可按 `a` 添加模型。表单收集 kind、配置名、base URL、模型 ID、密钥、输出上限和
上下文窗口；支持 `openai-chat`、`anthropic`、`ollama`。后端在落盘前用与启动相同的解析器校验全部模型，拒绝
重名或缺失必填字段，然后通过同目录临时文件、fsync、rename 原子更新 `models.json`，保留其 0600 权限。
整个「重读 → 校验 → 写入」由安装根 `run/models.lock` 的短期 flock 包围，两个前端同时添加模型不会丢失另一项。
添加成功后当前会话立即切换到新模型，`default` 不自动改变。`temperature`、`extra_body` 等高级字段仍可直接编辑 JSON。

`config/mcp.json` 独立保存 `mcpServers`、`connect_timeout_ms` 和 `probe_timeout_ms`，要求 0600。
stdio 项支持 `command`、`args`、`env`、`environment`，HTTP 项支持 `url`、`headers`；`${VAR}` 从进程环境展开。
两者都必须携带显式 `permissions` profile；stdio 在 SRT 内按范围启动，HTTP 要求 endpoint 网络目标获准。
`config/config.json` 不再允许重复声明 `mcp`。stdio 默认使用托管环境，可指定 `mcp/<name>` 或显式 `project`。

提示词、主题等配置路径相对于安装根；`--set key=value` 中的对应路径相对于 cwd。沙箱额外路径始终按下文的工作目录规则解析。未知键记 warning，类型和值错误
抛 `ConfigError`。`--set` 是一次性覆写，不写回配置；`--model` 命中名字时选择条目，否则临时覆写当前模型 ID。

`sandbox` 是独立版本化对象，当前只接受 `version: 2`、`backend: srt`、`host_access: ask_once`；
旧版配置被拒绝，不切换到旧后端。`extra_readable` / `extra_writable` 是宿主维护的持久范围，
相对路径按 workspace cwd 解析，不改变实际沙箱能力，也不覆盖控制数据、敏感读取或 `.git` 保护。
`allowed_targets/denied_targets` 表达网络目标；启动/网络审批超时与每执行网络请求预算由 sandbox 段配置。

主要运行策略由 `home/config/config.json` 显式提供，缺少必填项时报配置错误。
`run.max_total_tokens` 缺省为 0；`subagents.approval` 和 `web` 缺省采用下表值，兼容未添加这些字段的配置。

| 配置位置 | 用途 | 随附值 |
|---|---|---|
| `run.max_model_calls` | 每轮模型调用上限，0 不限 | 24 |
| `run.max_tool_calls` | 每轮工具调用上限，0 不限 | 35 |
| `run.max_total_tokens` | 本轮模型输入与输出总预算，0 不限；普通请求、父审阅、自动摘要及重试共享 | 0 |
| `run.max_model_retries` | 模型请求重试次数 | 2 |
| `run.max_parallel_tasks` | 子 Agent 并发数，必须为正整数 | 4 |
| `run.max_parallel_tools` | 只读工具并发数，必须为正整数 | 8 |
| `subagents.approval.mode` | `parent_when_unrestricted` 或 `user`，决定子请求是否可交父模型审阅 | parent_when_unrestricted |
| `subagents.approval.max_reviews_per_turn` | 每父 turn 审阅次数上限，0 表示不允许审阅 | 16 |
| `subagents.approval.review_timeout_ms` | 从请求创建起计时，包含排队、模型调用和重试；必须为正 | 60000 |
| `subagents.approval.failure` | 仅支持拒绝并反馈，不自动转人工 | deny_with_feedback |
| `web.timeout_ms` | 单次 SRT 网页请求上限，含等待审批；正整数 | 120000 |
| `web.startup_timeout_ms` | SearXNG 启动等待上限；正整数 | 30000 |
| `web.max_body_bytes` | HTTP 正文下载上限，1–67108864 | 2097152 |
| `web.cache_bytes` | 每个 Context 的页面缓存预算，至少等于下载上限 | 16777216 |
| `web.engines` | 非空的 SearXNG 引擎名称列表；配置中没有端口字段 | yahoo / brave / duckduckgo |
| `tools.bash_collect_bytes` | 中断时命令输出收集上限 | 4194304 |
| `ui.completion_max_files` | 每次文件补全扫描上限；每次查询读取当前目录 | 5000 |
| `session.history_scan_limit` | 每页历史最多扫描的记录数 | 100 |

上下文预算、工具结果与文件大小限制、HTTP/进程超时、日志大小及进度间隔也显式读取对应配置段。
`process.kill_grace_ms`、`process.drain_after_exit_ms`、`process.env_deny` 与 `log.also_stderr` 已列入配置文件。
HTTP 和进程配置同样传入 MCP；模型请求使用配置的总超时与空闲超时，不再强制覆盖。
web 工具的传输超时和正文预算来自独立的 `web` 段，输出上限仍由 `tools.max_result_bytes` 控制。
启动时解析宿主 curl 路径和 `internal/searxng` 环境目录；引擎名交由 SearXNG 启动时加载，详见 [web](web.md)。
子 Agent 在 `home/agents/*.md` 的 frontmatter 中设置 `max_model_calls`、`max_tool_calls`，省略或为 0 时继承全局配置。
模型输出预算统一设置在 `models.json` 每个模型的 `max_tokens`，必须大于 0；禁止通过 `extra_body` 再提供输出预算。
Chat 编码为 `max_completion_tokens`，Anthropic 编码为 `max_tokens`，Ollama 编码为 `options.num_predict`。

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
dagent runtime sync
dagent runtime list
dagent sandbox status

通用：-C/--cwd  -m/--model  --set  -r/--resume  --continue  --log-level
      --permissions <ask|workspace|unrestricted>  --read-only  --plan
run： --output <text|json|jsonl>
```

- `--read-only` 与权限三档正交；`--plan` 进入只读规划模式。
- `run` 可从 stdin 追加提示词；交互模式要求 stdin 是终端。
- `--continue` 选择当前 cwd 最近的会话，`--resume` 只接受当前 cwd 内的完整 ID 或唯一前缀。
- 退出码：0 成功，1 运行失败，2 参数/配置错误，130 中断。

- `sessions` 与 `--list-models` 也经独占后端查询（查询模式不创建会话、不启动 MCP、不收集 git 环境）；只有 `--help` / `--version`
  和参数错误不启动后端。

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
对外只返回 `PublicModel`。CMake 安装两个可执行文件和分目录资源，模型与 MCP 由不含私有凭据的模板首次生成，权限为 0600。

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
各格式共用主会话结果累积，在新步和流重试时重置正文缓冲，并从主会话 `turn_ended` 取得状态与用量。
子 Agent 事件不修改主会话结果，也不结束本次 run；JSONL 同样按最终状态返回退出码。

stdout 被关闭时 jsonl 请求取消本轮并返回 1，text / json 最终写出失败不改变运行状态。run 模式没有人工审批器：
主会话需要人工批准的调用返回「no interactive approver」工具错误，ask / exit_plan 返回原非交互文本。
符合 `subagents.approval` 与父权限条件的子请求仍可由父模型审批；没有可用路由时返回 approval unavailable，详见 [权限指南](../guide/permissions.md)。

### 信号与退出码

前端忽略 SIGPIPE，并在其他线程启动前屏蔽 SIGINT / SIGTERM，由 sigwait 线程处理。可优雅结束期间第一个信号经
`session.snapshot` + `run.cancel` 只取消本前端的当前 Run，第二个信号直接 `_Exit(130)`；加载配置、读取 stdin 等轮外阶段直接退出。
后端在独立进程组中运行，不接收终端信号。交互模式 raw 下的 Ctrl+C 是按键，行为由 ui 决定；全屏期间的进程信号走退出流程。

| 情况 | 退出码 |
| --- | --- |
| run 的 done；正常关闭交互；列表成功 | 0 |
| run 的 failed / limit / denied，启动或通信失败，jsonl 写出失败 | 1 |
| 参数或配置错误（包括后端返回 `config_error`） | 2 |
| run 被信号中断，或交互因进程中断信号退出 | 130 |

## 7. 全局技能

后端在启动时发现 `<root>/skills/*/SKILL.md`，把不可变目录共享给会话与查询网关。
格式、诊断和重启生效规则见 [skills](skills.md)。
