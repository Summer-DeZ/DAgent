# app：安装根、配置与命令行

头文件在 `src/public/app/`，实现在 `src/private/app/`，构建为 `dagent_app`。app 位于装配层：它认识各模块的
Options，外围模块不反向依赖它。

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
├── config.json
├── models.json       必须为 0600
├── system.md         主提示词
├── compact.md        压缩提示词
├── themes/
│   └── dagent.json
├── dagent.db         首次访问会话时创建
└── logs/
    └── dagent-<pid>.log
```

数据库和日志路径不可配置。日志带 pid，因此同一安装根的多个进程不会争用 rotating sink。

源码树中这些可管理文件统一放在 `home/`；dev 可执行文件无需设置环境变量就会读取这个目录。显式
`DAGENT_HOME` 仍然具有最高优先级。安装时 `home/` 中的配置、提示词和主题复制到安装根。

## 2. 配置与提示词

`config.json` 由用户维护，包含权限、MCP、UI、HTTP、上下文、工具、进程和日志策略；`models.json` 保存模型
条目和默认模型。加载不做分层合并，不读项目配置、`.mcp.json`、`.env` 或 XDG 路径，也没有信任子系统。
`prompts.system` 和 `prompts.compact` 分别指定主提示词与上下文压缩提示词；相对路径以安装根为准，默认是
`system.md` 与 `compact.md`。两份提示词都在启动会话时从文件读取，不再编入二进制，因此修改后无需重新构建。

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

`config.json` 的 MCP server 位于 `mcp.servers`。stdio 项支持 `command`、`args`、`env`，HTTP 项支持 `url`、
`headers`；字符串中的 `${VAR}` 从进程环境展开。`mcp.connect_timeout_ms` 与 `mcp.probe_timeout_ms` 和 servers
同处一个段。

配置中的相对路径统一相对于安装根；`--set key=value` 中的相对路径相对于 cwd。未知键记 warning，类型和值错误
抛 `ConfigError`。`--set` 是一次性覆写，不写回配置；`--model` 命中名字时选择条目，否则临时覆写当前模型 ID。

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

## 5. 装配

入口先解析 CLI，再解析安装根中的配置和提示词，随后把固定路径写入各模块 Options：

- `session::Options::database = <root>/dagent.db`
- `base::LogOptions::file = <root>/logs/dagent-<pid>.log`

交互模式关闭 stderr 日志 sink；headless 沿用配置。CMake 安装把可执行和两份 JSON 放到同一目录，并以 0600
安装 `models.json`。
