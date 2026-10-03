# 权限与沙箱

DAgent 的权限策略决定操作是否允许，SRT 负责受限进程的文件和网络边界。
模型 API 连接与远端 MCP 服务不等同于本机工具沙箱。部署步骤见 [构建与安装](build.md)。

## 模式

| 模式 | 普通工作区操作 |
| --- | --- |
| `ask` | 普通读取自动；写入和非只读命令需要审批 |
| `workspace` | 普通文件修改及具备 SRT 条件的工作区命令自动；界面称 auto-edit |
| `unrestricted` | 使用显式宿主权限；只读/规划上限和危险命令硬拦仍生效 |
| `--read-only` | 与上述三档正交，禁止写入、非只读命令和外部 MCP 调用 |
| `--plan` | 只读规划；交互确认后才进入执行模式 |

敏感文件、受保护路径、外部路径、网络目标和宿主执行分别判定，普通工作区许可不能覆盖这些边界。
`sandbox.extra_readable/extra_writable` 是持久路径配置；网络允许/拒绝项单独配置，拒绝优先。
完整分类和例外以 [权限契约](../design/agent.md#7-权限与沙箱) 为准。

## 审批与子 Agent

审批显示工具、cwd、权限模式、请求范围、来源 Agent 和是否已有部分执行。
单次授权只用于当前请求；会话授权只保存在当前会话内存中，可以在 `/permissions` 撤销。
撤销或降权会重新核对活跃执行，并停止不再获准的执行。敏感读取、受保护写入和 host access 不提供会话级复用。

子 Agent 不继承父的 unrestricted 或会话授权，只能在自身定义与父上限的交集中运行。
父取消会传到子执行；父权限收窄会取消超出新上限的子执行。子 Agent 的审批请求转交人工，
目前没有父模型自动批准子模型请求的功能；该功能仍是 [独立计划](../../nexttodo/parent-agent-approval-plan.md)。

子任务预算耗尽时返回 `status=limit`、`is_error=true`，已有输出标为 `Partial output`。
取消、拒绝和失败也会明确回传，不将过程文字当作成功结论。

## 排查顺序

```bash
dagent runtime list
dagent sandbox status
```

| 现象 | 处理 |
| --- | --- |
| `runtime list` 显示不匹配或依赖缺失 | 检查配置和锁文件后执行 `runtime sync` |
| 静态依赖通过但 `probe.ok=false` | 查看 probe 的阶段和错误；检查宿主 bubblewrap/socat 与 namespace 策略 |
| `loopback`、`uid_map` 或 namespace 拒绝 | 按构建指南部署匹配实际后端路径的 AppArmor profile，重新启动后端再探测 |
| `read-only SRT execution unavailable` | 先恢复 SRT 能力；glob/grep 也依赖只读 SRT，增加子 Agent 额度不能修复它 |
| `no interactive approver` | 当前入口没有人工审批器；改用交互入口，或提供适用的明确配置授权 |
| 模型连接失败 | 检查所选模型地址与服务状态，和 SRT 探测分开处理 |

SRT 是唯一受限执行后端，不保留旧 Landlock 回退。沙箱不可用时受限命令不启动；
符合策略的 Bash 请求可以询问一次性 host access，但不会自动变成宿主执行，
只读/规划限制也不会因此放宽。普通原生 `read` 成功不能证明 glob/grep 或 Bash 已可用。

`sandbox status` 会报告当前后端的 AppArmor 标签与可读取到的 userns 开关。
profile 的 `unconfined` 标记表示该 profile 提供 namespace 前置，具体工具隔离仍由 SRT 建立；
判断受限能力应看真实 `probe.ok`，执行记录再看实际 backend 与 sandbox。
