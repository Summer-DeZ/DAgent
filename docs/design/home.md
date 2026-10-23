# Home：自包含目录布局与资源分层

DAgent 采用**单一自包含根目录（Home）**管理所有配置、可编辑资源、运行时环境与持久化数据，不搜索 XDG、用户 `$HOME` 或项目下的隐藏配置文件。

## 1. 目录定位规则

根目录按以下优先级唯一确定（由前端解析后在初始化时传递给后端）：

1. 环境变量 `DAGENT_HOME`（若非空）。
2. 开发构建（`dev` preset）默认使用源码树下的 `home/`。
3. 正式构建默认使用可执行文件真实所在目录（解析符号链接后）。

## 2. 目录结构与职责划分

```text
home/
├── config/               用户配置（只读输入，除模型添加与外观设置外不自动改写）
│   ├── config.json       运行限制、权限、工具、UI、网络与日志策略
│   ├── models.json       生成模型与决策模型配置（含密钥，强制私有权限 0600）
│   ├── mcp.json          MCP 服务连接与显式沙箱权限配置（强制私有权限 0600）
│   ├── runtime.json      托管工具版本、MXC 原生库与独立依赖环境声明
│   ├── decision/         决策模型风险阈值配置模板
│   └── searxng/          托管 SearXNG 的 Python 依赖锁文件
├── AGENTS.md             可选的全局个人偏好指令
├── prompts/              主系统提示词（system.md）与上下文压缩提示词（compact.md）
├── skills/<name>/        全局技能包（SKILL.md 及脚本/参考资料）
├── agents/               子 Agent 角色定义（*.md）
├── themes/               UI 主题配色文件
├── memory/               Agent 跨会话记忆（global/ 与 projects/<dir-name>-<hash8>/）
├── data/                 不可丢弃的持久化数据
│   ├── dagent.db         SQLite 会话历史数据库
│   ├── decision.sqlite   决策模型审阅记录与动态阈值库
│   └── credentials/      OAuth 设备登录凭据（0700 目录，0600 文件）
├── runtime/              由 `dagent runtime sync` 准备好的工具二进制与依赖环境
├── cache/                公共模型清单缓存（opencode-go.json，24h 有效）与归档包缓存（packages/）
├── run/                  跨进程写锁（models/runtime/session-locks/credentials）与临时运行目录（searxng-*）
└── logs/                 按进程 PID 分离的滚动日志（dagent-<pid>.log）
```

**安全保护边界**：`config/`、`data/`、`logs/`、`run/`、`memory/` 属于受保护的控制面路径。即便工作区恰好位于 `home/`（如开发仓库），内置文件工具和命令沙箱也会拦截对这些目录的未授权读写。

## 3. 指令与上下文优先级

后端在构建会话上下文时，从多个来源收集指令，遵循以下严格优先级：

```text
当前用户请求 > 项目 AGENTS.md（由远及近） > 全局 home/AGENTS.md > Agent 记忆（memory）
```

- **系统提示词（`prompts/`）**：定义工具契约与核心行为准则，会话期间固定不变。
- **技能（`skills/`）**：启动时仅将名称与描述注入系统提示词，正文字段在某轮对话被显式或自动激活时按需注入。
- **记忆（`memory/`）**：模型自主维护的跨会话笔记，会话启动时扫描生成索引注入系统提示词，正文通过 `memory` 工具按需读取。

## 4. 安装、初始化与并发锁

### 4.1 首次安装与升级策略
- **非破坏性安装**：安装过程仅补充缺失的模板与默认资源，绝不覆盖用户已有的配置文件、提示词、主题、技能或历史数据库。
- **显式环境准备**：普通启动绝不后台静默下载工具或执行安装脚本。首次使用或更新 `runtime.json` 后，通过 `dagent runtime sync` 显式下载并校验托管工具链与沙箱原生库。

### 4.2 跨进程写锁协调
多个前端/后端实例共享同一个 Home 时，通过 `run/` 下的文件锁互不干扰地协调并发写入：
- `run/runtime.lock`：保护托管工具链同步（`runtime sync`）。
- `run/models.lock`：保护 `models.json` 与 `config.json` 外观设置的原子读改写。
- `run/session-locks/<id>.lock`：保护单个会话的独占可写执行权（不同会话可并发执行，只读查询不加锁）。
- `run/credentials/<provider>.lock`：保护登录凭据的并发刷新与写入。
