# app：安装根、配置与命令行

头文件在 `src/public/app/`，实现在 `src/private/app/`，分成两个库和两个入口，其他模块都不依赖它：

| 目标 | 内容 | 依赖 |
| --- | --- | --- |
| `dagent_app_cli` | 命令行解析、安装根、后端启动器、run 输出适配、进程信号 | client、protocol、ipc、base；CLI11 私有 |
| `dagent_app_config` | 配置/模型/子 Agent 定义读取、提示词渲染、会话与查询装配（实现 runtime 端口） | runtime、llm、tools、storage、workspace、exec、mcp、base |
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

```
<root>/
├── dagent
├── dagent-backend    前端从同目录启动
├── config.json
├── models.json       必须为 0600
├── system.md         主提示词
├── compact.md        压缩提示词
├── themes/
│   └── dagent.json
├── agents/           子 Agent 定义（*.md）
├── dagent.db         首次写入会话时创建
├── .runtime/         会话写锁与模型文件写锁
└── logs/
    └── dagent-<pid>.log
```

数据库和日志路径不可配置。日志带 pid（前后端各自一个文件），因此同一安装根的多个进程不会争用 rotating sink。
前端解析安装根后在 `app.initialize` 里交给后端；后端不自行决定安装根。

源码树中这些可管理文件统一放在 `home/`；dev 可执行文件无需设置环境变量就会读取这个目录。显式
`DAGENT_HOME` 仍然具有最高优先级。安装时 `home/` 中的配置、提示词和主题复制到安装根。

## 2. 配置与提示词

`config.json` 由用户维护，包含权限、MCP、UI、HTTP、上下文、工具、进程和日志策略；`models.json` 保存模型
条目和默认模型。加载不做分层合并，不读项目配置、`.mcp.json`、`.env` 或 XDG 路径，也没有信任子系统。
`prompts.system` 和 `prompts.compact` 分别指定主提示词与上下文压缩提示词；相对路径以安装根为准，默认是
`system.md` 与 `compact.md`。两份提示词都由后端在启动会话时从文件读取，不编入二进制，因此修改后无需重新构建。
只有后端读取 config.json、models.json、提示词与凭据；前端只读取初始化结果给出的 UI 主题文件。

`models.json`：

```json
{
  "default": "local",
  "models": {
    "local": {
      "kind": "openai-chat",
      "base_url": "http://127.0.0.1:10009/v1",
      "model": "Qwen3.8-Flash-Next",
      "context_window": 262144
    }
  }
}
```

模型的 `api_key` 可以直接保存，也可以写成 `env:VARIABLE_NAME`。启动时只从进程环境解析后一种形式。任何模型
列表只显示 key 的有无，不输出值。`models.json` 不是严格 0600 时拒绝启动并给出 `chmod 600` 提示。

交互界面的 `/model` 面板可按 `a` 添加模型。表单收集 kind、配置名、base URL、模型 ID、密钥、输出上限和
上下文窗口；支持 `openai-chat`、`anthropic`、`ollama`。后端在落盘前用与启动相同的解析器校验全部模型，拒绝
重名或缺失必填字段，然后通过同目录临时文件、fsync、rename 原子更新 `models.json`，保留其 0600 权限。
整个「重读 → 校验 → 写入」由安装根 `.runtime/models.lock` 的短期 flock 包围，两个前端同时添加模型不会丢失另一项。
添加成功后当前会话立即切换到新模型，`default` 不自动改变。`temperature`、`extra_body` 等高级字段仍可直接编辑 JSON。

`config.json` 的 MCP server 位于 `mcp.servers`。stdio 项支持 `command`、`args`、`env`，HTTP 项支持 `url`、
`headers`；字符串中的 `${VAR}` 从进程环境展开。`mcp.connect_timeout_ms` 与 `mcp.probe_timeout_ms` 和 servers
同处一个段。

配置中的相对路径统一相对于安装根；`--set key=value` 中的相对路径相对于 cwd。未知键记 warning，类型和值错误
抛 `ConfigError`。`--set` 是一次性覆写，不写回配置；`--model` 命中名字时选择条目，否则临时覆写当前模型 ID。

`sandbox` 是独立版本化对象，当前只接受 version 1；`extra_readable` / `extra_writable` 是宿主维护的持久范围，
相对路径按 workspace cwd 解析。它们只扩大兼容后端的显式范围，不改变 profile 能力结论，也不会覆盖控制数据、
敏感读取或 `.git` 保护。

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

1. 读取 config.json / models.json，按 argv 顺序应用 `--set` 与 `--model`；无效时返回 `config_error`，前端以 2 退出。
2. 初始化日志：`<root>/logs/dagent-<pid>.log`；交互模式关闭 stderr sink，run 与查询模式沿用配置。
3. 查询模式只构造配置网关与只读查询；interactive/run 模式另外收集一次工作区环境（git、AGENTS.md）、探测沙箱、
   读取子 Agent 定义与提示词，创建 `Assembly`（MCP Hub、环境事实、子 Agent 定义、模型表）与 `SessionAssembly`。
4. `SessionAssembly` 实现 `runtime::SessionFactory`：为每个会话渲染 system prompt、建立 `tools::Context` / Registry、
   打开记录写入器并取得写租约；存储路径固定为 `<root>/dagent.db`。

权限初值优先用 `--permissions`，再用 config.json，默认 workspace。凭据只在 app 内部的 `llm::ProviderConfig` 中出现，
对外只返回 `PublicModel`。CMake 安装把两个可执行文件、config.json、system.md、compact.md、themes/、agents/ 放到同一目录，
并以 0600 安装 `models.json`。

## 6. run 输出

run 模式的前端把协议事件经 `LegacyOutputCodec` 还原成原有公开输出，stdout 格式与退出码保持不变：

| `--output` | stdout | stderr |
| --- | --- | --- |
| `text` | 结束时打印最后一步正文，不流式写入 | 工具进度、Notice、重试及等待模型的心跳 |
| `json` | 结束时一个结果对象 | 同 text |
| `jsonl` | 第一行 session 元信息，随后实时事件，一行一个 JSON | 启动错误及 warn / error Notice；info 留在 jsonl |

json 结果字段为 `session_id, status, error, result, steps, tool_calls, usage, duration_ms`。jsonl 的首行为
`{"type":"session","id":"…","resumed":false}`，后续事件与原实时 JSON 同形，子 Agent 事件恢复原 `sub_event` 包装
（session / agent / parent_call / event）；协议新增的会话、操作与交互通知不进入公开输出。恢复时不输出历史。
text / json 的结果缓冲在新步和流重试时重置，使重试的半截输出不会混入最终结果。子 Agent 的 `turn_ended` 不结束本次 run。

stdout 被关闭时 jsonl 请求取消本轮并返回 1，text / json 最终写出失败不改变运行状态。run 模式没有交互审批器：
需要批准的调用返回「no interactive approver」工具错误，ask / exit_plan 返回原非交互文本。

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
