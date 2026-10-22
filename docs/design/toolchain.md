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
├── mxc/<version-generation>/        MXC 原生库（libmxc_ffi.so / mxc_ffi.dll）
├── envs/
│   ├── skills/<name>/<generation>/
│   ├── mcp/<name>/<generation>/
│   └── internal/searxng/<generation>/
├── homes/<profile>/
└── installed.json
```

`dagent runtime sync` 读取 `config/runtime.json`，准备缺失的版本，再原子替换 `installed.json`。
归档下载必须使用 HTTPS，并通过配置中的 SHA-256 后才解压；缓存命中也验证摘要。
包目录按版本、配置与平台生成，依赖环境按包快照、配置、锁文件及 package.json 内容生成。
准备成功写 `.ready.json`；失败时不发布新 manifest，重试可重建未完成目录。
已发布版本不在 sync 中修改或删除，运行中的后端继续使用启动时的路径，重启后使用新快照。

`app/toolchain_detail.hpp` 声明内部平台操作、共享命令执行和安装锁：

| 接口 | 实现与职责 |
| --- | --- |
| `execute_runtime_command` | `app/toolchain.cpp`：不继承宿主环境，沿用 exec 默认项与传入环境；超时 20 分钟，输出上限 65536，失败文本仍拼接程序名、stderr、stdout，成功时去掉尾部 CR/LF |
| `extract_runtime_archive` | `app/toolchain_posix.cpp`：在传入 cwd 执行 `/usr/bin/tar -xf <archive> --strip-components=<strip> -C <destination>`；`strip` 来自资产，默认 1 |
| `runtime_environment` | `app/toolchain_posix.cpp`：按原顺序构造 shell、PATH、HOME/XDG、包环境变量和可读目录 |
| `install_python_runtime` | `app/toolchain_posix.cpp`：设置准备环境，执行 uv python install/find，并创建 `bin/python` 和 `bin/python3` 符号链接，返回程序映射；Windows 返回实际解释器路径 |
| `select_python_runtime_program` | POSIX 保留配置相对路径的解析结果；Windows 采用安装器返回的解释器路径 |
| `python_environment_program` | `app/toolchain_posix.cpp`：返回环境根下的 `bin/python`，供 uv pip sync 和 SearXNG 启动使用 |
| `runtime_platform`、`SyncLock` | `app/toolchain_posix.cpp`：平台标识和原生安装锁 |

共享 `toolchain.cpp` 保留配置读取、下载和 SHA-256 校验、包顺序、代际与摘要计算、就绪检查和 manifest 发布；
归档验证完成后才调用解包接口。Python 包安装仍在非 Python 包之后，平台操作完成后再计算程序摘要、
写入 `.ready.json`；依赖环境就绪后才写最终 `installed.json`，不改变未完成代际的清理位置。
JSON 临时文件在原关闭位置调用 base::make_file_private；POSIX 仍执行原 filesystem::permissions，之后才 rename。
`runtime_platform` 保留 uname 的 `sysname-machine` 格式与失败异常；`SyncLock` 以不透明状态持有
`run/runtime.lock`，仍使用 0600/CLOEXEC、非阻塞独占锁，析构关闭文件，持有范围仍覆盖整个 sync 流程。

Windows 的 `toolchain_win.cpp` 提供 Windows-x86_64 平台标识和非阻塞 LockFileEx 同步锁，
锁文件创建时私有、句柄不继承、不共享删除，持有范围覆盖整个 sync，析构关闭释放。
解包明确调用 Windows 系统目录的 tar.exe（x64 上为 SystemRoot/System32），保持 -xf、`--strip-components=<strip>` 和 -C；
`strip` 来自资产且默认 1，Windows 自带的 bsdtar 因此也能解压 .zip。
运行环境不继承宿主 PATH；先按包顺序追加各包的 `bin` 目录，再追加 manifest `programs` 中各程序的父目录（去重），
最后追加系统目录和 Windows 目录。
Python venv 前置 Scripts，Node 环境前置 node_modules/.bin；HOME/USERPROFILE 指向所选 runtime/homes/<name>。
XDG、离线选项、uv/npm 缓存与配置变量保留原含义，增加 SystemRoot、系统 COMSPEC 及系统临时目录 TEMP/TMP。
环境构造不隐式创建目录，也不改变发布顺序。

Python 安装接口返回 RuntimePrograms 映射。Windows 保留 uv install/find 参数，--no-bin 不创建入口链接，
python/python3 均指向 uv find 返回的实际解释器；venv 使用 Scripts/python.exe。
共享层仍先验证配置的相对路径，再通过 select_python_runtime_program 选择实际程序；POSIX 保留原配置路径，
因此自定义 Python 程序路径、原符号链接目标和程序摘要不变。Windows 程序路径写入 JSON 时使用 UTF-8。
Windows 的系统 Bash/Git 与 uv/rg/node 归档由 `runtime.json` 的 `platforms` 覆盖登记；
这些平台接口不表示所有平台共用同一份包定义。

`dagent runtime list` 返回期望配置、安装记录和 `current`（平台、配置及依赖文件是否匹配）。
它不是磁盘完整性扫描；正常启动还检查程序是否可执行、依赖环境是否就绪。
修改配置或锁文件后必须重新 sync；执行命令和 MCP 启动都不隐式安装。

受限执行使用的 MXC 原生库是 runtime 的 `mxc` 包（见下文 `libraries`），由执行层在运行时加载；
bubblewrap、slirp4netns 等是宿主程序，不由 runtime 管理。
`runtime list` 不加载库、不验证 namespace；使用 `dagent sandbox status` 获取库版本与真实启动结果。

## 工具包配置

`version: 1`，`packages` 按名称声明包；常规包有 `version`、`bin` 和 `programs`（程序名到包内相对路径）。
`mxc` 包是例外，只声明 `version` 与 `libraries`。当前完整工具集要求提供 bash、git、rg、uv、python、python3、node、npm。

| kind | 来源与准备方式 |
| --- | --- |
| `archive`（默认） | `assets[平台]` 中的 `url`、`sha256`；tar 解压并移除 `strip` 层顶目录，`strip` 默认 1 |
| `files` | `files` 把本机路径复制到包内路径；`command:<name>` 只在显式准备时查询宿主 PATH |
| `python` | 先准备 uv，再由其安装指定 Python 版本到该包目录；不使用系统 Python |
| `system` | 不下载、不复制；`root` 指向宿主已有目录，不创建代际目录或 `.ready.json`，`entry.directory` 写 `root` |

`packages` 与 `environments` 的条目可带 `platforms`：键为 `runtime_platform()` 的结果（如 `Linux-x86_64`、
`Windows-x86_64`），值为对象或 `null`。当前平台有键且值为对象时，用该对象整体替换条目（不合并）；
值为 `null` 时删除该条目；没有当前平台键时只移除 `platforms` 键。解析在 Toolchain 读取配置后立即完成，
`configuration_id`、`status()`、`sync()` 和 `environments()` 都使用解析后的配置。不含 `platforms` 的配置
在解析后 dump 不变，因此摘要不变。

`assets.<platform>` 可带 `strip`（默认 1），作为 tar 的 `--strip-components` 值；没有顶层目录的 zip 用 0。
`system` 包要求 `root` 存在，否则报 `system package root is missing`；`bin`、`programs` 仍相对 `root` 校验，
程序摘要照常计算，两个 system 包可以共用同一个 root。

包可声明 `libraries`（库名到包内相对路径），登记执行层在运行时加载的原生库，此时 `programs` 可省略。
sync 只要求该文件存在、不检查可执行位，把绝对路径写入包记录与 `installed.json` 的 `libraries`，
由 `Toolchain::library(name)` 读取（未安装返回空路径）。库路径随平台或架构变化时沿用 `platforms` 整条覆盖：
`mxc` 包的基础条目描述 Linux-x86_64（`bin/x64/libmxc_ffi.so`），Linux-aarch64 与 Windows-x86_64 的覆盖条目
各自给出 `libraries`（`bin/arm64/libmxc_ffi.so`、`bin/x64/mxc_ffi.dll`）和指向同一 npm tgz、同一摘要的 `assets`。
该 tgz 是 npm 上的 `@microsoft/mxc-sdk@1.0.1`，按普通归档下载、校验 SHA-256 后解压（默认 `strip` 1 去掉 `package/` 顶层目录）；
执行层加载后还要求 `mxc_version()` 为 `1.0.1`，详见 [exec](exec.md#原生库与-c-abi)。

`env` 可为包增加执行变量，值开头的 `{root}` 替换为该包目录，例如 Git helper 与模板路径。
安装记录包含实际程序路径和准备时的程序摘要；下载包另记录归档摘要。
Python 发行包由固定 uv 版本的 managed Python 下载目录解析和校验。
POSIX 准备 Python 时，环境按 PATH、HOME、UV_NO_CONFIG、UV_PYTHON_INSTALL_DIR、UV_PYTHON_BIN_DIR、UV_CACHE_DIR
的顺序传入，PATH 固定 `/usr/bin:/bin`，缓存仍为 `HomePaths::cache / "uv"`。
先执行 `uv python install <version> --no-bin`，再执行 `uv python find --managed-python --no-python-downloads <version>`；
创建 bin 目录后，`bin/python` 指向查得程序相对 bin 的路径，`bin/python3` 的链接目标固定为 `python`。

仓库配置固定 Node、uv、ripgrep 和 Python 版本，并为 Linux aarch64、x86_64 提供归档。
Bash、Git 在 Linux 默认从准备机器导入快照，Git helper 路径采用 Ubuntu 布局；其它发行版应调整 `files`。
Windows 覆盖把 Bash、Git 登记为 `C:/Program Files/Git` 的 system 包，uv、rg、node 使用 Windows 归档，`mxc` 登记
`mxc_ffi.dll`；`internal/searxng` 在 Windows 解析为 null，不准备。
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
确有需要时可在该环境声明 `install_scripts: true`。注册的 npm 程序以 `.js` 结尾时，命令改为用托管 node
执行该脚本（Windows 的 `npm-cli.js`），否则直接执行 npm（Linux 的 `bin/npm`）。每个消费者拥有独立依赖目录。

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
POSIX 的 `runtime_environment` 对非 managed 环境先拼接 `<root>/bin:` 和 `<root>/node_modules/.bin:`，
再按 manifest 的 packages 遍历顺序追加各包 bin，最后追加 `/usr/bin:/bin`。
环境变量依次为 Python 环境的 VIRTUAL_ENV（如适用）、各包声明的 env、固定 PATH/HOME/XDG 等设置；
不去重、不调整覆盖顺序。shell 仍取 programs.bash，可读目录仍先放包目录、最后放环境根。
HOME/XDG、uv/npm 配置和缓存使用 home 内专用位置；不继承宿主 Python、Node、shell 初始化变量。
uv/npm 在普通执行时默认离线，防止常规启动顺带安装；显式命令仍受现有网络/执行权限控制。
MCP managed stdio 使用同一环境规则，cwd 为所选环境的根目录，server 的显式 `env` 在环境变量之后；
沙箱执行再把 HOME、XDG、TMPDIR 与 uv/npm 缓存改到每次执行私有的临时目录，见 [mcp](mcp.md#stdio)。

`project` 用于主动使用项目/宿主工具；非受限 bash 继承经已有 deny 过滤的宿主环境，
受限 bash 仍按沙箱清理环境。MCP project cwd 为工作目录。选择环境会进入命令意图，审批不跨环境复用。
默认文件搜索使用托管 rg，显式 `search.rg_path` 可覆盖搜索程序。Bash 环境提供托管 Git；
工作区上下文采集通过 `exec::system_git_program` 取得 POSIX 固定路径 `/usr/bin/git`，
在只读 MXC 沙箱内运行并禁用 fsmonitor/hooks/pager 和全局/系统配置；没有改为托管 Git 或 PATH 查找。

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

`internal/searxng` 是后端内部环境，不暴露为 bash/MCP 可选环境。它使用 managed Python，依赖通过
`config/searxng/requirements.lock` 的精确版本和 SHA-256 安装，只接受预编译 wheel。
环境声明可包含 `source`，其中的 HTTPS `url`、`sha256` 和固定 `commit` 进入环境代际标识；源码验证后解压到环境的 `source/`。
当前 SearXNG commit 是 `d48c4b555421e824342c51d68482dd0898e54d0f`，依赖锁从该提交的 requirements 解析。

`runtime sync` 只准备文件。`app::Searxng` 在第一次搜索时启动实际服务，随机密钥和 JSON/YAML settings 写到
`run/searxng-*/`，监听 `127.0.0.1` 的系统分配端口，以 `/healthz` 确认就绪；当前 Home 配置只选引擎，不写端口。
各后端互不共享实例。Child 管理进程组，正常退出时终止服务并清理 run 目录；搜索进程死亡后下次搜索重建。
当前正在执行的搜索若失败会如实返回，不在工具内自动重试。
Python 入口监听由后端持有写端的 stdin 管道，EOF 时结束进程组，后端异常退出也结束服务。
服务在宿主运行，网络地位与模型 API 类似；真正的工具连接仍是沙箱内 curl → 本次执行的回环代理 → 当前托管端点。
SearXNG 的 Python 入口通过 `python_environment_program` 取得，POSIX 仍为环境下的 `bin/python`。
代理变量通过 `base::environment_variable` 读取，依次透传 HTTP_PROXY、HTTPS_PROXY、ALL_PROXY、NO_PROXY
及对应小写变量；缺失时不追加，已设置为空时仍追加空值，不改变基础环境项与代理项的顺序。
`runtime sync` 准备依赖环境时（`uv venv`、`uv pip sync`、`npm ci`）以同样的顺序和规则把这八个宿主代理变量追加到托管环境之后，
使依赖下载与模型请求、SearXNG 一样跟随宿主代理；归档下载由进程内 libcurl 完成，按 libcurl 自身规则读取代理环境变量。

SearXNG 入口 `libexec/searxng_server.py` 的复制目标和安装规则仅在 Linux 启用，
入口随二进制更新，定位基准为后端路径。
SearXNG 用 `base::executable_path()` 定位入口，向 `base::create_unique_directory` 传入
`paths.run / "searxng-XXXXXX"` 路径模板创建实例目录，仍由调用方先创建父目录。目录清理仍由各自生命周期管理。
依赖准备与用户配置升级见 [构建指南](../guide/build.md#升级已有-home-的-web-资源)，引擎、缓存和传输选项见 [网页指南](../guide/web.md)。
