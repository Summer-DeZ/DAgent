# Home：配置、资源与状态

DAgent 只使用一个 home。`DAGENT_HOME` 优先；dev 构建默认源码树 `home/`，正式构建默认真实可执行文件所在目录。
前后端可执行文件始终放在同一目录；数据 home 可以通过 `DAGENT_HOME` 单独指定。路径由 `app::HomePaths` 统一生成，
不搜索 XDG、项目配置或其它用户目录。前端只决定根目录，后端负责读取配置和凭据。

```text
home/
├── config/
│   ├── config.json       行为、权限、工具、UI、日志策略
│   ├── models.json       模型与认证，0600
│   ├── mcp.json          MCP 连接与显式环境变量，0600
│   ├── runtime.json      工具版本、下载校验、独立依赖环境
│   └── sandbox/          SRT package.json 与锁文件
├── AGENTS.md             可选的全局用户指令
├── prompts/
│   ├── system.md         主提示词模板
│   └── compact.md        压缩提示词模板
├── skills/<name>/        SKILL.md、scripts、references、assets
├── agents/               子 Agent 定义
├── themes/               UI 主题
├── data/dagent.db        会话；SQLite WAL/SHM 与它同目录
├── logs/                 按进程记录日志
├── run/                  模型、runtime、会话写锁
├── runtime/              已准备的工具和依赖环境
└── cache/packages/       可重新下载的归档与包缓存
```

`config` 表达用户配置，`prompts/skills/agents/themes` 保存可编辑资源，`data` 保存不可丢弃的会话，
`run` 保存协调状态，`runtime` 保存实际执行环境。缓存不是会话或配置的来源。
提示词、主题等配置路径相对于 home，命令行 `--set` 中的对应路径相对于工作目录；
`sandbox.extra_readable/extra_writable` 始终相对于工作目录，详见 [配置契约](app.md#2-配置与提示词)。

## 用户指令

`home/AGENTS.md` 可放语言、协作方式等个人偏好，由后端追加到 system prompt。
项目 `AGENTS.md` 继续由工作区上下文收集；当前用户要求、项目具体指令优先于全局一般偏好。
提示词是系统模板，Skill 是按轮激活的操作指导，三者分别加载。编辑配置或资源后重启后端生效。

## 首次安装与升级

安装仅补充缺失的用户资源，不覆盖已有配置、提示词、主题、Skill 或子 Agent 定义。
二进制旁的 `libexec/srt_bridge.mjs` 随程序更新，`libexec/dagent.apparmor` 按最终安装路径重新生成；
两者不是 Home 下的可编辑用户资源。更换后端路径后需重新部署对应 profile。
源码的 `models.example.json`、`mcp.example.json` 安装为对应配置文件；本机真实认证文件不参与打包。
模型、MCP 配置必须为 0600。数据库在第一次写入会话时创建，包括其父目录。

新安装先编辑 `config/models.json` 和需要的 `config/mcp.json`，再执行：

```sh
dagent runtime sync
dagent runtime list
dagent sandbox status
dagent
```

首次准备需要下载。普通启动不安装工具、不执行包安装脚本；缺少已准备环境时给出 `runtime sync` 提示。
工具与依赖配置详见 [toolchain](toolchain.md)。
受限执行还需要宿主 bubblewrap/socat 与 namespace 能力，AppArmor 部署见 [构建与安装](../guide/build.md)。

## 唯一配置布局

启动和安装统一使用上述布局，不加载根目录旧 config.json/models.json，不提供旧布局迁移命令。
MCP 连接只使用 `config/mcp.json` 中的 `mcpServers`，不转换旧 `servers` 字段。
每个 server 必须携带显式 `permissions` profile，否则不启动：

```json
{
  "mcpServers": {
    "note": {
      "type": "stdio",
      "command": "/abs/path/node",
      "args": ["/abs/path/server.mjs"],
      "environment": "managed",
      "permissions": {
        "read": ["/abs/dir"],
        "write": ["/abs/dir"],
        "network": ["api.example.com:443"]
      }
    }
  }
}
```

stdio server 在 SRT 内按该范围启动（网络是严格 allowlist）；HTTP server 的 `network` 必须显式列出
endpoint 目标。
个人模型配置和凭据保存在 `config/models.json`。新安装没有数据库时由首次写入自动创建；
之后的新会话追加到已有数据库，升级和重启不清空历史。

runtime sync 使用 `run/runtime.lock`，模型写入和会话写入分别使用
`run/models.lock` 和 `run/session-locks/<id>.lock`。锁只协调各自负责的数据。
