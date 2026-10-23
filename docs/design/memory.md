# memory：Agent 跨会话记忆

`memory` 机制（由 `tools::MemoryStore` 与 `app` 查询网关实现）支持主 Agent 将用户偏好、协作反馈与项目关键决策以结构化 Markdown 笔记的形式跨会话持久化。它与人工维护的 `AGENTS.md` 分离，由模型通过内置 `memory` 工具自主维护，用户也可通过 `/memory` 面板或直接编辑文件管理。

**指令优先级**：`当前用户要求 > 项目 AGENTS.md > 全局 AGENTS.md > Agent 记忆`。

## 1. 存储结构与条目模型

记忆按「全局（跨项目）」与「当前项目」两个作用域存储在 `<home>/memory/` 下：

```text
home/memory/
├── global/<name>.md                         跨项目的个人偏好与通用约定
└── projects/<dir-name>-<hash8>/<name>.md    当前项目专属决策（目录名 + 规范化路径 SHA-256 前 8 位）
```

每条记忆为一个独立的 `.md` 普通文件（单条上限 `max_entry_bytes` 默认 4 KiB），采用 YAML Frontmatter（`name` 为 kebab-case、`description` 单行非空、`updated` 为后端自动生成的 `YYYY-MM-DD` 日期、`type`）+ Markdown 正文格式：

| 类型（`type`） | 记录内容 |
| --- | --- |
| `user` | 用户身份、技术背景、沟通风格偏好 |
| `feedback` | 用户纠正或认可的做法及原因（正文包含 Why 与 How to apply） |
| `project` | 无法从代码或 Git 历史直接推出的架构决策、约束与目标 |
| `reference` | 外部文档、看板、工单或数据源位置 |

**存储安全边界**：所有读写与扫描操作均固定目录句柄并拒绝跟随任何符号链接或 FIFO 等非普通文件；写入采用同目录临时文件 + `fsync` + 原子重命名。不维护单独的索引文件，手工增删改文件会在下次会话启动时自动生效。

## 2. 关键流程：两阶段加载（索引注入 + 按需读取）

为控制上下文开销，记忆采用「启动注入轻量索引 + 运行时按需读取正文」的两阶段设计：

```mermaid
flowchart LR
    SCAN[会话新建/恢复时扫描目录] --> IDX[按预算裁剪生成索引]
    IDX --> SYS[注入 System Prompt 的 # Memory 段]
    SYS --> LLM[模型根据索引判断相关性]
    LLM -->|需要详情或更新| TOOL["调用 memory(view / save / delete)"]
```

1. **会话启动扫描与预算裁剪**：创建或恢复主会话时，扫描 `global` 与当前 `project` 目录，生成仅含 `name / type / updated / description` 的索引列表并追加到系统提示词末尾。若超出 `max_index_bytes`（默认 8 KiB），按 `(scope, updated, name)` 升序排列（利用 `"global" < "project"` 字典序），优先从头淘汰最旧的全局条目，其次淘汰最旧的项目条目。
2. **会话期间前缀固定**：本会话运行期间新增或修改记忆不会动态改写当前会话的 `system` 提示词（保持前缀缓存稳定），新索引将在下次新建或恢复会话时生效。
3. **子 Agent 隔离**：子 Agent 不注入记忆索引，也不提供 `memory` 工具；父 Agent 需将子任务相关的背景事实直接写入委派提示词。

## 3. 工具语义与权限设计

- **内置工具接口**：`memory(op="view" | "save" | "delete", scope="global" | "project", name, ...)`。
- **权限与调度**：`memory` 属于受控的专用工具类型（`ToolKind::memory`），在所有权限模式（含 `read_only` 与 `plan`）下均直接放行、无需审批，且始终串行执行。
- **控制面路径保护**：`home/memory/` 目录本身属于受保护的控制面路径；模型若试图用通用文件工具（`read`/`write`/`edit`/`bash`）直接操作该目录，仍会触发受保护路径拦截。

## 4. 用户管理

用户可通过交互界面的 `/memory` 面板直接浏览和删除记忆（不消耗模型调用，也不改变当前已打开会话的系统提示词）：
- 前端通过 `memory.list`、`memory.view`、`memory.delete` 三个只读/管理 RPC 与后端查询线程交互。
- 即便在配置中将 `memory.enabled` 设为 `false`（关闭模型侧的索引注入与工具注册），用户管理面板依然可用。
