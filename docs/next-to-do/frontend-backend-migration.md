# DAgent 整体等价重构：范围、行为基线与总体决策

状态：已实施（R01–R14 真实验收，见 [执行记录](implementation-tasks.md#7-执行记录)）。基线：`d614700`。本文件与 [对象设计](architecture-refactor.md)、
[状态机](execution-state-machines.md)、[记录路线](record-routes.md)、[协议](frontend-backend-protocol.md)、
[任务与验收](implementation-tasks.md) 共同构成规格。

## 1. 目标与优先级

用户最终要求是：保留当前功能边界，全面处理耦合过高、抽象不完整的问题，遵守面向对象基本原则，并完成 UI 与执行后端分离。

实施优先级：

1. 用户当前明确约束和项目 AGENTS.md。
2. 本文的功能保持清单及范围限制。
3. 对象、状态和协议契约。
4. 任务文件中的实施顺序。
5. 当前设计文档中的背景说明。

本组文档替换早期草案，不再采用共享本地 daemon、长期子 Agent、多轮子任务、自动重连、控制权租约、后台继续执行等设计。
当前源码是行为证据；遇到源码与旧设计文档不一致，先看本文已核实的条目，再定位实际实现，不自动采用旧文字。

## 2. 要解决的设计问题

| 编号 | 已确认问题 | 改造结果 | 可检查的结构证据 |
| --- | --- | --- | --- |
| D01 | Shell 同时负责控件、队列、Agent 创建/恢复/替换和状态同步 | 会话控制归 Runtime，UI 只持有客户端与展示状态 | UI 无 Agent、Setup、Recorder、会话数据库调用 |
| D02 | Agent 同时组合环境、循环、调度、权限、记录和子 Agent 反向访问 | Session、Run、TurnRunner、ActionDispatcher 职责分离 | 无公开 setup()/current_turn()；构造依赖明确 |
| D03 | ask/exit_plan 的 run 是错误占位，dispatch 解释具体交互选项 | 普通工具与控制动作拥有各自完整执行契约 | 普通 PreparedTool 全部可执行；控制语义不在调度循环 |
| D04 | 实时执行多处分别改 Conversation、写 Recorder、发 Sink | SessionCommitter 统一提交变化并发布事实 | 一项变化只有一个提交入口和一份转换语义 |
| D05 | replay_into 同时恢复运行与生成历史展示 | 共用记录解码，分离 Recovery 与 HistoryProjector | 历史查询无恢复写入、无 Agent/MCP 构造 |
| D06 | Todo 主要通过 View 维持界面状态 | WorkPlan 属于会话，结果与历史生成其投影 | UI 不能成为计划唯一持有者 |
| D07 | Setup 混合配置、运行状态、密钥和对象引用 | 拆分配置值、运行状态、构造依赖 | 核心无总括环境对象，协议无 Setup/ProviderConfig 原样序列化 |
| D08 | task 工具持有父 Agent，调用栈隐式决定寿命 | 显式 DelegationContext 与 SubagentExecutor | 子执行只获得所需上下文，父等待结束后统一回收 |

D08 只整理已有单次委派的所有权，不创建长期 Task 产品能力。

## 3. 范围决定

### 3.1 必须保留

- 当前 TUI 页面、快捷键、英文 chrome、工具卡片和思考折叠。
- `dagent`、`run`、`sessions`、`--list-models` 与现有通用选项。
- 所有已注册工具的名称、模型侧 JSON Schema、提示说明和结果语义。
- 模型 provider、当前权限三档、read-only/plan、沙箱后端选择与授权规则。
- 现有模型重试、上下文预算、裁剪/摘要、会话记录及崩溃闭合。
- 单层、单次子 Agent 委派与当前并发上限。
- 当前安装根、配置文件和 SQLite 历史兼容。

### 3.2 不实施

- 长期、多轮、递归或独立后台子 Agent；新的 task.start/send/wait/cancel 模型工具。
- 新的面向用户 CLI/斜杠命令、队列暂停功能、独立子任务取消入口。
- 多前端共享后端、连接发现、监听路径、控制租约、客户端接管、远程网络。
- 自动重连、事件补发缓存、快照重同步、持久 command 去重、持久输入队列。
- 通用 actor 框架、协程运行引擎、动态插件系统、全局资源调度平台。
- 新模型协议、权限功能、沙箱行为、主题能力或 UI 原语。
- 无关缺陷修复、清理原有 test 目录、修改本地模型服务状态。

`dagent-backend` 是分离架构必需的正式内部配套进程，不引入供用户操作的 serve/status/attach/detach 子命令。

### 3.3 进程与会话范围

每个前端启动一个独占后端。一个后端有一个当前顶层可执行会话及其本轮子会话；切换顶层会话只在空闲时进行。
以前会话作为持久历史存在，不保留为后台活跃任务。子 Pane 仅为只读展示。
同一安装根下可以有多个独立前后端对，但同一个 session_id 同时只能有一个可写执行所有者。
前端退出或私有连接断开时取消当前执行、清空未开始输入、保存并退出后端，不继续后台运行。

## 4. 功能保持清单

下列 B 编号是实现和验收的稳定引用。不可把“重构后看起来更合理”的行为替换进来。

| 编号 | 保持契约 | 当前证据 |
| --- | --- | --- |
| B01 | CLI 名称、参数解析、stdin 合并、--resume/--continue 互斥与退出码保持 | `app/cli.cpp`、`agent/headless.cpp` |
| B02 | 安装根解析、配置文件格式、模型密钥 env: 解析和 models.json 权限规则保持 | `app/config.cpp` |
| B03 | --set / --model 按 argv 顺序处理；路径和会话归属使用精确规范化 cwd，进程不 chdir | `app/cli.cpp`、`agent/main.cpp` |
| B04 | 普通输入 FIFO；空输入时向上键取回最后一条仍排队输入 | `ui/shell.cpp` 的 submit/drain/recall |
| B05 | 斜杠命令立即分派，不进入普通输入队列；各命令保留自己的 busy 条件 | `ui/shell.cpp` 的 register_commands/submit |
| B06 | 当前 TurnEnded 不论 status 都会清 busy 并 drain；手动压缩完成也 drain | `ui/shell.cpp` 的 apply/compact |
| B07 | 新建/恢复/切模型/手动压缩在 busy 时不开始；新会话清 planning，沿用当前权限档 | `ui/shell.cpp` 对应方法 |
| B08 | 权限档运行中可切换，对之后决策生效；plan 切换要求空闲；read-only 与档位正交 | `agent/permission.cpp`、`ui/shell.cpp` |
| B09 | ask 每轮至多 3 次；非交互 ask/exit_plan 和审批缺失按原结果返回，不等待外部用户 | `agent/dispatch.cpp` |
| B10 | plan 接受选项对应 workspace / ask / 继续规划；执行端决定模式与只读状态 | `agent/dispatch.cpp` 的 exit_plan 分支 |
| B11 | Esc、Ctrl+C、关闭浮层/清输入/返回主 Pane/连续退出的顺序与现有快捷键保持 | `ui/shell.cpp` 的 interrupt/cancel/exit |
| B12 | 子 Pane 不能提交输入；保留用户草稿并给出当前提示 | `ui/shell.cpp` 的 submit/show_pane |
| B13 | 切模型仍使用同一 session_id、保留当前模式；按现有恢复语义重置会话临时授权、FileTracker 和 token 校准，重新加载持久对话 | `ui/shell.cpp` 的 switch_model、`agent.cpp` 的 resume/构造 |
| B14 | 模型切换失败保留旧会话；现有 Todo 显示不因成功切模型清空 | 同上及 `ui/transcript.cpp` |
| B15 | 只读组最多 8；task 组使用 max_parallel_tasks；按块等待，结果按原调用顺序提交 | `agent/dispatch.cpp` |
| B16 | 默认 max_model_calls=24、max_tool_calls=35、max_model_retries=2，task 并行默认 4，配置校验保持 | `agent/options.hpp`、`app/config.cpp` |
| B17 | 达到工具预算后的最终总结机会、模型重试、空回复及 finish_reason 处理保持 | `agent/agent.cpp`、`agent/model.cpp` |
| B18 | task 参数与单次执行保持；父等待结果；子不能 task/ask/exit_plan；默认子工具不含 MCP，显式配置可允许 | `agent/subagent.cpp` |
| B19 | 子权限只收窄；创建时读取父当前策略；取消 token 贯穿父子；不新增运行中自动同步子权限政策 | `agent/subagent.cpp`、`agent/permission.cpp` |
| B20 | 主会话控制 MCP 等待/刷新/重连，子仅使用创建时快照；重连次数与超时保持 | `agent/mcp_hub.cpp` |
| B21 | 每次 todo 完整替换计划；名称、Schema、模型文本和 View 持久形状保持 | `tools/todo.cpp`、`tools/view.cpp` |
| B22 | Conversation 的消息配对、不切断工具组、压缩取消回滚和摘要规则保持 | `agent/conversation.cpp`、`agent/compaction.cpp` |
| B23 | Recorder 写入失败进入 broken，仅通知一次，当前回合继续；不默改为失败即停止 | `agent/record.cpp`、`agent/agent.cpp` |
| B24 | 恢复未闭合调用为“结果未知/中断”，不自动重跑；恢复时按当前环境重新构建 system prompt | `agent/record.cpp`、`agent/agent.cpp` |
| B25 | text stdout 仅最终正文；json 字段、jsonl 首条 session 及事件形状保持；进度仍去 stderr | `agent/headless.cpp`、`agent/events.cpp` |
| B26 | jsonl 输出失败取消当前运行并返回 1；text/json 末尾输出失败保持现有处理，不借机改退出码规则 | `agent/headless.cpp` |
| B27 | 模型添加字段、校验、0600 原子落盘、成功后切换且不改变 default 的行为保持 | `app/config.cpp`、`ui/shell.cpp` |
| B28 | TUI 主题/布局/补全/工具和思考折叠、MCP 状态与子会话只读浏览保持 | `ui/*`、`docs/design/ui.md` |

B06、B13、B23 是容易被“顺便优化”改掉的行为。本次保留，不额外加入失败暂停队列、切模型保留临时授权、磁盘失败停机等策略。
模型的随机正文、时间戳、运行时 ID、输出分块边界不要求逐字节一致；功能结果、结构、不变式和公开输出字段要求一致。

## 5. 本次允许的结构性行为差异

以下差异是实现解耦的必要边界，不构成新功能：

- 历史只读查询不再调用完整执行恢复逻辑；返回原有可显示事实，不改变会话内容。
- 同一会话被另一个后端占用时，明确报告正在使用，不能启动第二个写入者。
- 私有连接丢失视作本次前端终止：后端取消收尾，前端报告连接/执行失败；不重试提交。
- 取消确认与运行终态分开，UI 不在后端实际收尾之前伪造结束。
- 切换模型的 B13 重置语义由会话控制显式实施，不要求 UI 通过销毁整个 Agent 达成。
- 查询和交互回答不排在阻塞模型调用之后，保证分离架构下仍可操作。

不列在这里且无法由 B 清单证明的产品行为修改，记录为后续议题；不能自主实施或把它作为本计划前置条件。

## 6. 用户路径保持

1. 作为用户，我希望原命令启动原界面，以便原有使用方式不变。
2. 作为用户，我希望连续输入和取回保持原顺序，以便控制下一轮内容。
3. 作为用户，我希望审批、拒绝、反馈和模式切换保持，以便明确执行权限。
4. 作为用户，我希望工具输出、计划、上下文和子会话继续可见，以便了解正在做的事。
5. 作为用户，我希望新建、恢复、切模型和压缩保持，以便继续管理已有会话。
6. 作为用户，我希望中断、退出和崩溃恢复保持，以便处理未完成工作。
7. 作为脚本用户，我希望 stdout 数据格式与退出码保持，以便现有调用不需改写。
8. 作为维护者，我希望可独立理解核心与 UI，以便后续修改集中在职责明确的位置。

## 7. 冻结的工程决定

| 事项 | 决定 |
| --- | --- |
| 进程 | `dagent` + 它启动的 `dagent-backend`；一对一 |
| 传输 | 私有 AF_UNIX socketpair，UTF-8 JSON Lines，JSON-RPC 2.0 |
| 运行方式 | 保留阻塞核心循环及现有工具组线程，不引入协程或全局任务调度 |
| 会话并发 | 每后端一个活跃顶层会话；已有子 Agent 组并发保持 |
| 排队 | Runtime 内存队列；启动执行时才创建 Run，不增加持久队列 |
| 持久化 | 保持现有 SQLite schema、events 类型与 payload 兼容；本次无需 runtime_* 新表 |
| Run/交互身份 | 后端实例内有效，不承诺重启恢复或跨连接去重 |
| UI 状态 | 草稿、页面、滚动、折叠属前端；执行、模式、已提交输入属后端 |
| 部署 | 安装两个正式二进制到同一根；现有资源路径保持 |
| 迁移 | 先整理核心，再包装后端，再切换前端；最终删除过渡入口 |
| 验证 | 正式构建与真实模型/工具/TUI运行；不写任何测试代码 |

## 8. 设计落地的完成标准

仅增加 socket 或拆文件不算完成。必须同时满足 D01–D08 的结构结果、B01–B28 的功能保持、
L01–L23 的记录路径、协议生命周期以及 [全部任务的实际验收](implementation-tasks.md)。
文档状态、构建结果和运行证据分别记录；不能以模型声称完成替代真实工件。
