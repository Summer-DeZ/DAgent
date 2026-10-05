# Toolchain：托管工具与独立依赖

这里的 runtime 指 home 下的工具环境；`src/*/runtime` 仍负责会话控制。
`app::Toolchain` 读取期望配置、显式准备环境并提供不可变执行快照。exec 接收环境变量和程序路径，
tools、MCP、workspace 使用快照执行，不负责下载和安装。

## 目录与生命周期

```text
runtime/
├── bash/<version-generation>/
├── git/<version-generation>/
├── uv/<version-generation>/
├── python/<version-generation>/
├── node/<version-generation>/
├── rg/<version-generation>/
├── envs/
│   ├── skills/<name>/<generation>/
│   ├── mcp/<name>/<generation>/
│   ├── internal/sandbox/<generation>/
│   └── internal/searxng/<generation>/
├── homes/<profile>/
└── installed.json
```

`dagent runtime sync` 读取 `config/runtime.json`，准备缺失的版本，再原子替换 `installed.json`。
归档下载必须使用 HTTPS，并通过配置中的 SHA-256 后才解压；缓存命中也验证摘要。
包目录按版本、配置与平台生成，依赖环境按包快照、配置、锁文件及 package.json 内容生成。
准备成功写 `.ready.json`；失败时不发布新 manifest，重试可重建未完成目录。
已发布版本不在 sync 中修改或删除，运行中的后端继续使用启动时的路径，重启后使用新快照。

`dagent runtime list` 返回期望配置、安装记录和 `current`（平台、配置及依赖文件是否匹配）。
它不是磁盘完整性扫描；正常启动还检查程序是否可执行、依赖环境是否就绪。
修改配置或锁文件后必须重新 sync；执行命令和 MCP 启动都不隐式安装。

内部环境 `internal/sandbox` 固定 SRT 0.0.77，声明与锁文件位于 `config/sandbox/`。
它供执行层使用，不是模型可选的 Bash 环境。Node/SRT 由 runtime 管理，bubblewrap 和 socat 是宿主程序。
bridge 位于后端二进制旁的 `libexec/`，跟随程序版本而不是 Home 的依赖快照更新。
`runtime list` 不验证 namespace；使用 `dagent sandbox status` 获取真实启动结果。

## 工具包配置

`version: 1`，`packages` 按名称声明包；每个包有 `version`、`bin` 和 `programs`（程序名到包内相对路径）。
当前完整工具集要求提供 bash、git、rg、uv、python、python3、node、npm。

| kind | 来源与准备方式 |
| --- | --- |
| `archive`（默认） | `assets[平台]` 中的 `url`、`sha256`；tar 解压并移除一层顶目录 |
| `files` | `files` 把本机路径复制到包内路径；`command:<name>` 只在显式准备时查询宿主 PATH |
| `python` | 先准备 uv，再由其安装指定 Python 版本到该包目录；不使用系统 Python |

`env` 可为包增加执行变量，值开头的 `{root}` 替换为该包目录，例如 Git helper 与模板路径。
安装记录包含实际程序路径和准备时的程序摘要；下载包另记录归档摘要。
Python 发行包由固定 uv 版本的 managed Python 下载目录解析和校验。

仓库配置固定 Node、uv、ripgrep 和 Python 版本，并为 Linux aarch64、x86_64 提供归档。
Bash、Git 默认从准备机器导入快照，Git helper 路径采用 Ubuntu 布局；其它发行版应调整 `files`。
需要重新导入时修改该包 `version`，不能靠反复 sync 更新同名已发布快照。

## 独立依赖环境

`environments` 用 `skills/<skill-name>`、`mcp/<server-name>` 或 `internal/<name>` 标识独立环境。
`internal/*` 仅供后端内部使用，不提供给模型选择。用户可配置的消费者环境例如：

```json
{
  "skills/report": {
    "kind": "python",
    "lockfile": "skills/report/requirements.lock"
  },
  "mcp/filesystem": {
    "kind": "node",
    "package_json": "dependencies/filesystem/package.json",
    "lockfile": "dependencies/filesystem/package-lock.json"
  }
}
```

这些路径相对于 home，依赖声明文件由用户维护。Python 锁文件采用带精确版本及哈希的 requirements 格式，
使用托管 Python 创建 venv，然后执行 `uv pip sync --require-hashes --only-binary=:all:`。
不隐式调用系统编译器构建源码包。Node 复制 manifest/lock 后执行 `npm ci`，默认禁用安装脚本；
确有需要时可在该环境声明 `install_scripts: true`。每个消费者拥有独立依赖目录。

匹配 `skills/<name>` 时，Skill 的本轮指令会附加对应 bash 环境提示。调用方式：

```json
{"command":"python /absolute/path/to/skills/report/scripts/report.py","environment":"skills/report"}
```

MCP 在 `config/mcp.json` 的 `mcpServers` 条目选择环境，并声明启动权限；下面是单个条目的示例：

```json
{
  "type": "stdio",
  "command": "mcp-server-filesystem",
  "args": ["/absolute/workspace"],
  "environment": "mcp/filesystem",
  "permissions": {
    "read": ["/absolute/workspace"],
    "write": [],
    "network": []
  }
}
```

依赖准备成功不代表允许启动，也不代表协议兼容；server 还须具备显式 profile、所需沙箱能力，
并支持 DAgent 当前 [MCP 协议](mcp.md)。

## 执行环境与边界

bash 默认 `environment: managed`，也可选独立依赖环境或显式 `project`。
managed 从空进程环境开始，使用托管 Bash `--noprofile --norc`；PATH 先放该消费者的依赖，再放托管工具。
HOME/XDG、uv/npm 配置和缓存使用 home 内专用位置；不继承宿主 Python、Node、shell 初始化变量。
uv/npm 在普通执行时默认离线，防止常规启动顺带安装；显式命令仍受现有网络/执行权限控制。
MCP managed stdio 使用同一环境规则和 home cwd，server 的显式 `env` 最后覆盖。

`project` 用于主动使用项目/宿主工具；非受限 bash 继承经已有 deny 过滤的宿主环境，
受限 bash 仍按沙箱清理环境。MCP project cwd 为工作目录。选择环境会进入命令意图，审批不跨环境复用。
默认文件搜索使用托管 rg，显式 `search.rg_path` 可覆盖搜索程序。Bash 环境提供托管 Git；
工作区上下文采集当前仍使用 `/usr/bin/git`，在只读 SRT 内运行并禁用 fsmonitor/hooks/pager 和全局/系统配置。

托管目录进入沙箱可读范围并禁止工具写入；config、data、logs、run 保持控制数据保护。
环境管理不等于安全沙箱，命令仍遵守原权限与 OS 隔离规则。

当前仍依赖 Linux 内核、动态加载器/系统库及基础 POSIX 工具；PATH 尾部保留 `/usr/bin:/bin`。
初次准备需要 `/usr/bin/tar` 及 gzip/xz 解压能力，导入 Bash/Git 时也需要本机来源。
因此解决的是 Python/Node 版本、用户环境污染及消费者依赖冲突，不是完全脱离操作系统。
已准备目录包含绝对路径和 venv shebang，不支持直接搬家；迁移到新路径时复制配置/资源/数据并重新准备 runtime。
目前没有自动更新、版本回收或卸载命令，旧版本保留供正在运行的后端使用。

上游行为依据：[uv managed Python](https://docs.astral.sh/uv/guides/install-python/)、
[uv 环境变量](https://docs.astral.sh/uv/reference/environment/)、[Node 官方发行包](https://nodejs.org/en/download)。

## 托管搜索基础设施

`internal/searxng` 与 sandbox 同级，不暴露为 bash/MCP 可选环境。它使用 managed Python，依赖通过
`config/searxng/requirements.lock` 的精确版本和 SHA-256 安装，只接受预编译 wheel。
环境声明可包含 `source`，其中的 HTTPS `url`、`sha256` 和固定 `commit` 进入环境代际标识；源码验证后解压到环境的 `source/`。
当前 SearXNG commit 是 `d48c4b555421e824342c51d68482dd0898e54d0f`，依赖锁从该提交的 requirements 解析。

`runtime sync` 只准备文件。`app::Searxng` 在第一次搜索时启动实际服务，随机密钥和 JSON/YAML settings 写到
`run/searxng-*/`，监听 `127.0.0.1` 的系统分配端口，以 `/healthz` 确认就绪；当前 Home 配置只选引擎，不写端口。
各后端互不共享实例。Child 管理进程组，正常退出时终止服务并清理 run 目录；搜索进程死亡后下次搜索重建。
当前正在执行的搜索若失败会如实返回，不在工具内自动重试。
Python 入口监听由后端持有写端的 stdin 管道，EOF 时结束进程组，后端异常退出也结束服务。
服务在宿主运行，网络地位与模型 API 类似；真正的工具连接仍是 SRT 内 curl → 当前托管端点。

SearXNG 入口 `libexec/searxng_server.py` 与 SRT bridge 一样随二进制更新，定位基准为后端路径。
依赖准备与用户配置升级见 [构建指南](../guide/build.md#升级已有-home-的-web-资源)，引擎、缓存和传输选项见 [网页指南](../guide/web.md)。
