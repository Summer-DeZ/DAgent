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
普通目录的读取授权也不能覆盖其中的敏感文件。父批准的会话范围按资源和操作类型匹配，
例如普通工作区写入授权可用于后续 write/edit，目录读取授权可用于其普通子文件。

子 Agent 不继承父的 unrestricted 或会话授权，只能在自身定义与父上限的交集中运行。
父取消会传到子执行；父权限收窄会取消超出新上限的子执行。父当前为 unrestricted、没有只读/规划上限，且
`subagents.approval.mode=parent_when_unrestricted` 时，可审批的子请求交给父模型审阅。其余情况沿用人工审批。
审阅显示 `Reviewing subagent permission`，不弹出人工审批框；headless 运行同样可用。硬拒绝不能被父模型覆盖。

父批准可以限定一次调用或当前子会话；兄弟任务不共享授权。host access 始终只限一次，并且表示宿主全访问，
不会伪装成仅放行某个域名。记录以 `authority=parent_model` 区分父模型与 `user`，保存范围、理由和实际执行后端。
父降权或取消使待审阅与尚未消费的授权失效，并取消依赖被撤销父权限的活跃执行；已发生的副作用无法回滚。

受信任 Home 的 `config.json` 配置示例：

```json
{
  "subagents": {
    "approval": {
      "mode": "parent_when_unrestricted",
      "max_reviews_per_turn": 16,
      "review_timeout_ms": 60000,
      "failure": "deny_with_feedback"
    }
  }
}
```

将 mode 设为 `user` 可保留人工审批。父审阅使用父当前模型，计入同一 turn 的模型调用与 token 用量；超时、无效输出、
模型错误或预算耗尽均拒绝并向子任务反馈，不自动转人工弹窗。恢复历史只展示审批证据，不重授临时权限。
只有明确的网络拒绝才形成会话拒绝规则；审阅错误、超时、取消或预算不足不会永久否决后续同域请求。

`run.max_total_tokens=0` 表示不设 turn token 上限；配置为正数后，普通父模型请求、审阅、自动摘要及其重试共享剩余额度。
`run.max_model_calls` 按实际请求尝试计数，每次重试发送前重新检查次数与 token 预算。
服务端未返回用量的失败或中断请求按输入及已收到输出估算消耗，扣减后续预算；审阅另记录 `estimated_budget_tokens`。
`usage` 和 `turn_end.usage` 仍只报告服务端提供的实际用量，估算值不会伪装成实测 token 数。

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
| `no interactive approver` | 当前请求走人工路由而入口没有人工审批器；改用交互入口或明确配置。父审批仅接手符合上述条件的子请求 |
| 模型连接失败 | 检查所选模型地址与服务状态，和 SRT 探测分开处理 |

SRT 是唯一受限执行后端，不保留旧 Landlock 回退。沙箱不可用时受限命令不启动；
符合策略的 Bash 请求可以询问一次性 host access，但不会自动变成宿主执行，
只读/规划限制也不会因此放宽。普通原生 `read` 成功不能证明 glob/grep 或 Bash 已可用。

`sandbox status` 会报告当前后端的 AppArmor 标签与可读取到的 userns 开关。
profile 的 `unconfined` 标记表示该 profile 提供 namespace 前置，具体工具隔离仍由 SRT 建立；
判断受限能力应看真实 `probe.ok`，执行记录再看实际 backend 与 sandbox。
