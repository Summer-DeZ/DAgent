# 权限与沙箱

DAgent 的权限策略决定操作是否允许，SRT 负责受限进程的文件和网络边界。
模型 API 连接与远端 MCP 服务不等同于本机工具沙箱。部署步骤见 [构建与安装](build.md)。

## 模式

| 模式 | 普通工作区操作 |
| --- | --- |
| `ask` | 普通读取自动；写入和非只读命令需要审批 |
| `workspace` | 普通文件修改及具备 SRT 条件的工作区命令自动；界面称 auto-edit |
| `unrestricted` | bash 使用宿主权限，web 仍使用只读 SRT；只读/规划上限和危险命令硬拦仍生效 |
| `--read-only` | 与上述三档正交，禁止写入、非只读命令和外部 MCP 调用 |
| `--plan` | 只读规划；交互确认后才进入执行模式 |

进入 unrestricted 之前会先弹出风险警告，用户确认后才升权；取消则留在 auto-edit。
每个会话首次切入时提示一次，同一会话再次进入不再提示，状态栏保留 error 色提示。
以 unrestricted 启动 TUI 时同样在启动阶段提示；headless（run）不弹框，只向 stderr 打印同样内容的警告并立即启用。

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
父取消会传到子执行；父权限收窄会取消超出新上限的子执行。父当前没有只读/规划上限、模式为 auto-edit 或
unrestricted，且 `subagents.approval.mode` 允许该模式时，可审批的子请求先交给父模型审阅；
父拒绝、超时、模型失败转人工，父停止、请求取消或权限失效直接结束。
`parent_when_workspace_or_unrestricted`（新默认）覆盖两种模式，`parent_when_unrestricted` 只覆盖 unrestricted，
`user` 完全交给人工。host access 仍只允许无只读/规划上限的 unrestricted 父复批，否则转人工。
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

将 mode 设为 `user` 可保留人工审批。父审阅使用父当前模型，计入同一 turn 的模型调用与 token 用量；普通拒绝、超时、无效输出、
模型错误或预算耗尽进入人工审批；父停止、请求取消或权限失效直接结束旧请求。没有人工出口时返回审批不可用，不等待。
恢复历史只展示审批证据，不重授临时权限。
`review_timeout_ms` 从请求创建起计时，包含多个子任务排队和模型重试；`max_reviews_per_turn=0` 禁止父模型审阅，
不表示不限次数。当前审阅只能完整批准原请求或拒绝，不能返回缩小后的资源子集。
只有明确的网络拒绝才形成会话拒绝规则；审阅错误、超时、取消或预算不足不会永久否决后续同域请求。

`run.max_total_tokens=0` 表示不设 turn token 上限；配置为正数后，普通父模型请求、审阅、自动摘要及其重试共享剩余额度。
`run.max_model_calls` 按实际请求尝试计数，每次重试发送前重新检查次数与 token 预算。
服务端未返回用量的失败或中断请求按输入及已收到输出估算消耗，扣减后续预算；审阅另记录 `estimated_budget_tokens`。
`usage` 和 `turn_end.usage` 仍只报告服务端提供的实际用量，估算值不会伪装成实测 token 数。

子任务预算耗尽时返回 `status=limit`、`is_error=true`，已有输出标为 `Partial output`。
取消、拒绝和失败也会明确回传，不将过程文字当作成功结论。

## 决策模型代批（auto-review）

auto-edit（workspace）下可以配置决策模型（第一版为 System One 协议上的 d3）。它只在计划列出的场景里代批，
不覆盖硬拒绝、不扩大写入范围，也不改变 ask / unrestricted / 只读的既有语义；未配置时全部保持原行为。

| 场景 | 交给决策模型的证据 | 判定维度 |
| --- | --- | --- |
| 普通命令联网 | 用户原话、命令、host:port | relevance、direction |
| 明确目标脚本联网（本会话未修改） | 完整命令、目标路径 | relevance |
| 目标脚本联网（本会话修改过） | 上述内容加该脚本相对 HEAD 的 diff | relevance、net_change，涉网时再问 direction |
| 工作区外普通文件读取 | 路径与用户任务（不读正文） | relevance、privacy |
| MCP 调用 | 工具名与调用参数 | relevance、direction |
| web_fetch 每一跳 URL | 当前完整 URL（含重定向后的下一跳） | relevance、direction |
| web_fetch 结果 | 按切片抽样的正文 | injection（固定阈值，从严） |

判定顺序：配置/会话拒绝 → 修改脚本的强制复审 → 已放行 → 决策模型代批 → 人工（子会话先父复批）。
`fetch` / `send` 需要所选类别概率与对应 relevance 同时达标；`modify` / `unknown` 不代批。
网络批准只对本次执行的该 host:port 有效，MCP 只对这一次调用有效，都不生成会话规则；
决策模型批准的工作区外文件读取保存文件级会话授权，可在 `/permissions` 撤销。

`fetch_url` 的 URL 批准只覆盖这一跳；同 host 的不同 URL 仍重新审阅，拒绝后当前 web_fetch 结束。
网页结果扫描发现注入或扫描失败时，该次 web_fetch 不返回正文，并关闭本次用户请求整轮（含所有子 Agent）的自动代批；
之后需要审批的请求直接交给人工，直到下一个用户请求开始新一轮。

界面把决策模型的结果显示为 `auto-review`，只显示来源、结果与固定理由，不显示分数。
审阅证据、各维度分数与人工最终决定保存在 `data/decision.sqlite` 的独立记录中；会话记录只保留
`authority=decision_model`、结果、理由与审阅编号。阈值按模型/问句/配置版本隔离，未标定场景一律转人工。

```json
{
  "decision": "d3-local",
  "decision_models": {
    "d3-local": {
      "kind": "systemone",
      "endpoint": "http://127.0.0.1:10006/v1/systemone",
      "model": "d3",
      "model_revision": "deployment-1"
    }
  }
}
```

以上放在 `config/models.json`；`decision: null` 或缺省表示禁用。行为配置在 `config/config.json`：
`decision_review.timeout_ms`（默认 10000）与 `decision_review.threshold_profile`（默认 default）。
决策选择在进程启动时生效，切换模型需重启后端。

## 网页搜索与抓取

`web_search` 和 `web_fetch` 在 ask、workspace、unrestricted、read_only 和 plan 下都可调用，但始终要求只读 SRT。
ask/workspace/read_only/plan 中，新目标走已有网络审批；headless 无审批路由时明确失败。
子 Agent 还受 `agents/*.md` 的工具名单约束，只读权限不会自动向它添加 web 工具。
unrestricted 对新目标自动放行，配置 `sandbox.denied_targets` 和会话拒绝仍优先。收窄权限会终止失去授权的活跃连接。

网页里批准 `docs.python.org:443` 的本会话访问后，bash 再连接它无需重复**网络**审批；ask 下命令本身仍可能需要审批。
重定向每跳单独判定，用户拒绝后当前调用结束，同目标在本会话内直接拒绝。`/permissions` 可撤销允许或拒绝规则。

搜索服务由每个后端懒启动，使用独立的动态 loopback 端口，不配置固定端口。
这个基础设施端点仅在 web_search 当前调用内预授权，仍受配置拒绝约束。SearXNG 的上游请求在宿主运行，
属于 harness 基础设施；外部搜索引擎不逐个进入工具沙箱审批。
`sandbox.allowed_targets/denied_targets` 控制 SRT 工具连接，不是全后端网络防火墙；它们不约束宿主 bash、模型 API 或 SearXNG 的上游连接。
抓取和搜索结果都是不可信资料。使用、配置及排障见 [网页指南](web.md)，实现边界见 [web 设计](../design/web.md)。

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
| `read-only SRT execution unavailable` | 先恢复 SRT 能力；glob/grep 和两个 web 工具也依赖只读 SRT，增加子 Agent 额度不能修复它 |
| `no interactive approver` / `Permission approval unavailable` | 当前请求没有可用审批路由；改用交互入口或明确配置。父审批仅接手符合上述条件的子请求 |
| 模型连接失败 | 检查所选模型地址与服务状态，和 SRT 探测分开处理 |

SRT 是唯一受限执行后端，不保留旧 Landlock 回退。沙箱不可用时受限命令不启动；
符合策略的 Bash 请求可以询问一次性 host access，但不会自动变成宿主执行，
只读/规划限制也不会因此放宽。普通原生 `read` 成功不能证明 glob/grep 或 Bash 已可用。

`sandbox status` 会报告当前后端的 AppArmor 标签与可读取到的 userns 开关。
profile 的 `unconfined` 标记表示该 profile 提供 namespace 前置，具体工具隔离仍由 SRT 建立；
判断受限能力应看真实 `probe.ok`，执行记录再看实际 backend 与 sandbox。
