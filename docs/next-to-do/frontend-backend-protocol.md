# 私有前后端协议：现有功能的通信契约

状态：已实施（R01–R14 真实验收，见 [执行记录](implementation-tasks.md#7-执行记录)）。遵循 [功能边界](frontend-backend-migration.md)，对应 [对象](architecture-refactor.md) 和 [状态机](execution-state-machines.md)。
这份协议是现有功能的进程间适配，不提供共享后端、后台任务、自动重连或新的面向用户接口。

## 1. 进程与传输

前端创建 AF_UNIX/SOCK_STREAM socketpair，启动同安装目录下的 `dagent-backend --ipc-fd <fd>`。
这个参数只用于正式前端装配，后端无此 FD 时报告用法错误，不开放 serve/listen 模式。
前端持有一端，子进程只继承另一端；不经 shell 启动，不通过 argv 传密钥或完整提示词。

- 所有无关 FD 为 CLOEXEC；传递的 IPC FD 进入后端后设为 CLOEXEC，工具/MCP 子进程不能继承它。
- 后端不读取前端终端 stdin；已有 run stdin 由前端 CLI 合并后通过请求发送。
- 后端不参与终端渲染；日志使用现有文件 sink，用户可见诊断通过 DTO 返回前端处理。
- 前端拥有后端 PID 和回收责任。后端新建独立进程组，终端 SIGINT 先由前端转成现有取消语义，不广播到其他后端。
- socket EOF 表示本前端结束：后端清队列并取消收尾，不继续执行。
- 不建立监听 socket、发现文件、client lease 或跨连接认证系统；私有 FD 的创建和继承关系就是通信边界。

消息采用 UTF-8 JSON Lines，一行一个完整 JSON-RPC 2.0 对象，字符串中的换行按 JSON 转义。
读取处理半帧和多帧，写入处理短写。只支持对象消息，不支持 batch。
请求包含字符串 id；通知不包含 id；响应以同 id 匹配，不要求跨独立查询按请求顺序返回。
基础规则见 [JSON-RPC 2.0 官方规范](https://www.jsonrpc.org/specification)。

## 2. 启动与关闭时序

### 2.1 启动

1. CLI 解析 --help/--version/语法错误时直接返回，不启动后端。
2. 解析安装根并启动后端，建立私有连接。
3. `backend.hello` 交换业务协议版本。握手必须在 10 秒内完成；超时只清理本前端创建的后端。
4. `app.initialize` 发送启动模式、cwd、按原顺序的覆写、permissions/read_only/plan、resume/continue 选择。
5. 后端加载现有配置；interactive/run 模式创建或恢复会话，查询模式只准备查询所需资源。
6. 初始化返回有效会话/配置摘要、UI 主题文件路径与必要 Notice。历史用独立 history 查询取得。
7. TUI 初始会话及历史准备成功后进入全屏；run 输出原有首条 session 数据后提交用户输入。

只有后端读取 models.json、凭据、提示词和业务配置。前端可以读取返回的纯 UI 主题文件。
环境变量由子进程正常继承，后端仍按原 env:KEY 规则解析；不新增“上传环境”协议。
初始化可能读取真实历史，不能用握手的 10 秒限时把它误判为业务失败；长请求允许取消/关闭但不自动重发。

### 2.2 关闭

正常退出使用 `backend.shutdown`：停止接受输入、清队列、取消父子执行、终结交互、join、同步、关闭 MCP/连接、退出。
前端在请求后等待响应/进程退出并恢复终端。宽限 10 秒后可终止自己创建的后端并报告未干净结束。
后端 EOF、前端强退与正常关闭使用同一个后端清理入口，不各写一套业务收尾。
后端意外退出时，前端报告错误并结束或回到错误展示，不自动拉起新后端并重发最后输入。

## 3. 版本与身份

业务版本为 major=1、minor=0；与 JSON-RPC 的固定 jsonrpc="2.0" 分开。
两端随同一发行安装，major/minor 不相等直接返回 version_mismatch，本次不做多版本能力协商框架。

| 身份 | 范围与用途 |
| --- | --- |
| request id | 一次连接中的请求匹配；单调生成字符串；不作持久去重 |
| backend_instance_id | 本次后端启动身份，用于诊断和运行身份前缀 |
| session_id | 现有持久会话 ID |
| session_generation | new/resume/模型替换成功后的递增代数，拒绝过时修改和丢弃错页响应；普通权限/plan 变化不递增 |
| input_id | 后端内存队列的一条输入；用于原子取回 |
| run_id | 当前后端内一次 turn/compact；实例前缀加计数即可，不写新数据库表 |
| invocation_id | 一次 Run 内的动作身份，区分步骤和调用序号 |
| model_call_id | provider 原有 tool_call id；写原记录和兼容 jsonl 时保持 |
| interaction_id | 一次待回答交互，Run 结束后不能复用 |
| seq | 当前连接下已发布事件的递增序号；不与 SQLite events.seq 混用 |

本协议不重试有副作用的业务请求。收到响应只表示其定义的接受/完成状态，不代表持久化成功。
响应丢失或连接关闭时不换 id 再发一次；后端会关闭，用户下次显式恢复会话。
不增加持久 command_id、runtime_runs 或 runtime_interactions 表。

## 4. 请求方法

所有会话修改请求都携带目标 session_id 和当前 session_generation，避免迟到操作落到新会话。
new/resume 本身针对当前 generation；只读查询明确目标 session_id，可以查询历史子会话。

| 方法 | 输入 | 返回与执行规则 |
| --- | --- | --- |
| backend.hello | protocol_version、frontend_version | backend_instance_id、实际版本 |
| app.initialize | root、cwd、mode、ordered_overrides、CLI 选择 | 启动结果；只允许一次，后续不能替换安装根 |
| backend.shutdown | 无 | 清理完成或连接结束；重复关闭幂等 |
| session.snapshot | 当前 session_id | 当前只读状态、generation、state_seq |
| session.new | 当前 generation | 空闲创建并切换；busy 返回 busy |
| session.resume | 当前 generation、目标完整 ID/前缀/continue 选择 | 按精确 cwd 解析并切换；失败保留旧对象 |
| session.list | cwd、limit、cursor 可选 | 现有顶层会话列表，子会话不混入 |
| session.children | session_id | 既有持久子关系和本轮已知子会话摘要 |
| session.history | session_id、cursor 可选、upper_seq 可选、limit | 只读 HistoryItem 页、固定高水位、next_cursor |
| session.history_close | cursor | 释放未读完的只读历史查询；重复关闭无副作用，不关闭会话 |
| input.submit | 当前身份、text | input_id；表示加入内存队列，不表示已经执行/保存 |
| input.recall_last | 当前身份 | 原子取回最后一条仍 queued 的输入；空则返回无结果 |
| run.cancel | 当前 session_id、run_id | 已请求取消或已结束；不能取消历史/子会话的独立运行 |
| session.compact | 当前身份 | 空闲开始压缩，返回 operation_id；完成走 operation.finished |
| session.cycle_permission | 当前身份 | 在后端按 ask → workspace → unrestricted → ask 前进一步，返回生效快照；运行中可用 |
| session.toggle_planning | 当前身份 | 后端读取当前值并切换；空闲生效，busy 拒绝 |
| session.grants | 当前身份 | 现有临时授权列表；界面可用条件保持 B05 |
| session.revoke_grant | 当前身份、grant_id | 空闲撤销结果；审计仍走 Session 提交出口 |
| model.list | 无 | 公开模型描述、has_key、当前选择；无原始凭据 |
| model.add | 现有表单字段及 write-only credential | 保持原保存与随后切换行为，失败保留原选择 |
| session.select_model | 当前身份、模型配置名 | 后端执行 B13/B14 模型切换 |
| workspace.info | 当前 session_id | cwd、项目路径、git 摘要 |
| workspace.complete | 当前 session_id、查询文本、limit | 当前文件候选；前端保留防抖/排序/选择 |
| interaction.answer | interaction_id、answer | 回答已接受或已关闭；不直接返回/接受 Grant |

这些是内部 IPC 方法，不对应新增 CLI 或模型工具。斜杠命令仍由 UI 按现有注册表映射。
主题/滚动/折叠/帮助/面板选择等纯 UI 操作不发业务请求。
没有 config.refresh、control.claim、queue.pause、task.send、stream.resume 等方法。

循环权限/切换规划发送业务意图，不能由前端基于可能滞后的快照计算下一个值后重复覆盖。
连续快捷键按后端接收顺序各执行一次；CLI 指定的初始权限仍通过 initialize 设置。
前端可维护 pending-request 状态用于防止同一个面板操作重复提交，但它不替代后端 busy/权限真值。

session.history 的 cursor 是本次连接内的后端查询游标，包含恢复验证所需的上下文关联；不是前端自行计算的页号。
完整分页、释放和失败规则见 [记录路线 §7](record-routes.md#7-分页读取的完整契约)。关闭历史查询是内部资源回收，不新增用户命令。

### 4.1 接受与完成

- input.submit 响应只确认队列接受；实际开始通过 TurnStarted/状态事件表示。
- new/resume/select_model 可耗时，其最终响应在成功/失败后返回；通信读线程仍可处理取消/关闭和其他只读请求。
- 手动 compact 返回 operation_id，结果通知结束 busy；不增加旧 jsonl 中不存在的 turn 起止事件。
- model.add 只在空闲执行，后端依次完成保存和现有自动切换。文件保存成功但切换失败时如实返回公开模型已添加及切换错误，不能回滚用户模型文件假装未保存。
- 查询线程失败只影响该查询，不结束模型 Run。
- 对未知请求 ID 的迟到响应忽略并记录身份，不把它视为新的状态更新。

### 4.2 公共输入与返回形状

SessionTarget 固定为 `{session_id:string, session_generation:integer}`；表中“当前身份”均指它。
所有 ID 字段为字符串；seq/generation/计数为整数；可空字段用 JSON null，集合为空用 []，不混用缺失/空字符串表达状态。
可选请求字段允许省略；请求中不认识的控制字段返回参数错误，不忽略一个可能改变动作含义的拼写错误。

| 方法组 | 固定结果 |
| --- | --- |
| hello | protocol_version={major:1,minor:0}、backend_version、backend_instance_id |
| initialize | mode、session:SessionSnapshot或null、resumed:boolean、ui={theme_file:string或null}、progress_interval_ms |
| new/resume/select_model/cycle_permission/toggle_planning | 成功返回 SessionSnapshot；失败返回统一RPC error，不另发一份成功响应 |
| session.snapshot | SessionSnapshot |
| input.submit | input_id、accepted=true；由发送队列先排入接受响应，再允许该输入的TurnStarted入队 |
| input.recall_last | input:null 或 {input_id,text}；取回即从后端队列移除 |
| run.cancel | run_id、result=cancel_requested或already_finished；不是运行终态 |
| compact | operation_id、accepted=true；OperationFinished含 operation_id/status/error |
| grants | grants数组，每项id/description |
| revoke_grant | grant_id、removed:boolean |
| model.list | models数组、default_name、selected_name或null；每项使用PublicModel |
| model.add | model:PublicModel、selected:boolean、selection_error:string或null；落盘失败仍为RPC error |
| session.list / children | sessions数组、next_cursor或null；每项为现有Summary的公开值字段 |
| session.history | session_id、upper_seq、items:HistoryItem数组、next_cursor或null |
| history_close | closed=true；该查询已释放也返回true |
| workspace.info | cwd、project_root、branch；无git信息时branch为空字符串 |
| workspace.complete | candidates数组、truncated:boolean；候选含 path、directory:boolean |
| interaction.answer | interaction_id、accepted=true；已经关闭使用interaction_closed错误 |
| shutdown | closed=true；只在实际收尾后返回，随后关闭连接 |

initialize 的业务输入固定为 root/cwd绝对路径、mode=interactive/run/sessions/models、ordered_overrides:string数组、
可选permissions、read_only、plan、可选resume_id、continue_last、可选log_level。缺省布尔值为false；
ordered_overrides保留现有CLI解析得到的顺序与内部@model标记。prompt由后续input.submit传递，output格式留前端。
mode选择不增加业务能力；query模式的initialize不创建可写会话，session=null。

SessionSnapshot 必需字段固定为 session_id、session_generation、state_seq、model、permission_mode、planning、read_only、
busy、current_operation或null、queue数组、context、work_plan、mcp数组、children数组、recording={broken,error}。
current_operation含kind=turn/compact/replacing、可选run_id、phase；busy由该对象是否存在导出，不另存一份业务布尔值。
queue项为input_id/text_preview；完整文本仅在取回时返回。context含used/limit/usage，work_plan保持原todo条目形状。
error无错误时为空字符串；可选run_id用null。model含PublicModel公开字段，没有api_key。

业务错误只用第8节固定形状；不要有的方法返回error字段、有的方法抛RPC error。
唯一例外是model.add的已保存但切换失败，以及RunOutcome自身的业务error，两者语义已明确区分。

## 5. 事件与快照

事件通知方法为 `event`，params 包含 seq、session_generation、session_id、可选 run_id/invocation_id、kind、data。
子事件还包含 parent_session_id、parent_invocation_id、model_call_id；归属由后端决定，不能按当前 UI 页面猜测。

| kind 组 | 内容 | 前端用途 |
| --- | --- | --- |
| session.changed / queue.changed | 权威会话摘要、pending 输入摘要 | 标签、busy、队列提示 |
| turn_started / step_started / turn_ended | 现有回合事件 | 显示和本轮结果 |
| text / reasoning / stream_reset / retrying | 现有流式增量及重试 | 当前消息投影 |
| tool_pending / tool_started / tool_output / tool_finished | 现有工具事件、完整授权展示事实和结果 | 工具卡片 |
| context / compacted / model_changed / mode_changed | 现有变化 | 状态栏、模型与模式标签 |
| notice | 现有提示和 broken 信息 | Toast/错误展示 |
| interaction.requested / interaction.closed | 带身份的审批/问答 | 对话框 |
| mcp.changed | 原 MCP 状态快照的变化 | 替代跨进程轮询对象 |
| operation.finished | 手动压缩等操作完成 | 结束对应 busy，不驱动历史回放 |

只对执行事实发送上述实时通知；历史内容放在查询结果中。
现有 SubEvent 在新 IPC 中展平身份；CLI 的旧 JSONL 输出适配器重新包装成原有 sub_event 格式。

SessionSnapshot 包含：当前 session_id/generation、模型公开信息、permission/planning/read_only、当前操作/Run、
排队输入摘要、上下文、WorkPlan、MCP、已知子会话、记录 broken 状态。
不包含 Conversation 可写结构、Policy 规则容器、模型密钥或实时流完整缓存。

初始快照和随后事件在同一个 Publisher 顺序边界发布：snapshot.state_seq=S，之后应用 seq>S 的事件。
前端不把旧快照覆盖已经应用的更新状态。没有断线后重新订阅/补发承诺。
历史按固定 upper_seq 查询，返回条目带稳定 record_seq/ordinal 标识；HistoryItem 只进入 Transcript 历史方法，不进入实时控制方法。

该顺序边界的实现固定为：Publisher持有只读ViewState与事件序号；发布变化时在同一短锁中更新ViewState、分配seq并把不可变消息入发送队列。
生成快照响应也在此锁下捕获ViewState与S并排入同一队列；锁内不写socket、不读数据库、不等待发送容量。
若大消息队列容量不足，先在锁外等待容量，再重新进入锁捕获当前快照，不能等待期间保留旧S。
进度生产者同样先获得可取消的容量许可，再持短锁分配seq和入队；两步之间关闭则释放许可并结束投递。
前端按session_generation路由；只读历史响应不能替换当前执行状态，旧generation的修改响应不能覆盖新会话。

## 6. DTO 与现有展示的对应

| DTO | 至少包含的现有信息 |
| --- | --- |
| PublicModel | 配置名、kind、模型名、base_url、输出/窗口预算、has_key；不含 api_key |
| ModelInput | 原模型表单所有输入；credential 是仅此次请求可写字段 |
| ToolPresentation | read 范围、文件 diff/计数、bash 命令/输出/退出码/沙箱事实、grep/glob 结果、MCP 内容、todo/ask/task 摘要 |
| ApprovalRequest | tool、agent 来源、reason、summary、preview_kind/preview_text、cwd、mode、requests、session_rule、can_network、partially_executed |
| QuestionRequest | header、prompt、选项 label/description、multi_select、allow_other |
| ApprovalAnswer | allow / allow_session / deny / deny_with_feedback、feedback、network |
| QuestionAnswer | 原选项索引列表、other、cancelled；后端校验后转换为控制动作语义 |
| HistoryItem | 用户/assistant/思考/工具结果/模型变化/结束信息，保留原可显示内容 |

preview_kind 只表示 text/code/diff，不包含 TUI BlockKind 或主题令牌。
ToolPresentation 是语义事实，UI 决定折叠、颜色、布局。存储 View JSON 与协议 DTO 分开编码，不能因换前端而改变旧记录格式。
数据默认尽量沿用现有字段含义，不创建同义的多套结构。

## 7. 并发、队列与背压

RPC 接收线程、发送线程与执行线程分开。所有输出经一个有序发送队列，不能让多个工具线程同时写同一个 socket。

发送队列采用 8 MiB 软上限：达到后阻塞生产者等待可用空间，但等待必须可取消，不能持业务锁。
大于上限的一条合法历史/结果消息仅在队列为空时单独发送；其大小由现有业务数据限制，协议不增加新的用户输入硬限制。
因此队列上界为“8 MiB 或当前单条大消息中的较大者”，不宣称是严格 8 MiB。
发送器分块写 socket，接收器按 LF 增量组帧，不依赖一次 read/write 完成整包。

必要的文本增量可以在事件编号之前合并相邻同来源片段；结果/重置/结束事件之前必须排出此前片段。
保留每个 bash 调用的 UTF-8 边界缓冲，避免将一次字符跨字节块切分误替换；这是现有 JsonlOutput 的行为迁移。

控制接收不依赖发送队列腾空。取消/关闭唤醒所有等待队列的生产者，丢弃关闭后无法送达的进度并先完成资源清理。
健康连接不丢完成结果，不引入“丢事件后靠重连修复”。前端停止消费导致执行被背压减速是允许的，但不能阻塞取消路径。
历史默认每页扫描 100 条持久记录，显示项可能更少；游标按记录推进。单个大条目完整保留，不增加历史截断。

## 8. 错误与兼容

JSON-RPC 标准解析/参数/未知方法错误使用标准码。
业务错误使用 code=-32000，data.kind 固定为：busy、stale_session、not_found、session_in_use、
interaction_closed、invalid_state、config_error、query_failed、startup_failed、version_mismatch、closing。
message 是供用户阅读的说明；前端只按 data.kind 分支，不解析文案。

- 模型/工具业务失败属于 RunOutcome/ToolResult，不包装成 RPC 协议失败。
- 前端参数或配置错误返回 2；执行/启动/通信失败返回 1；成功 0；用户中断 130。模型运行结果细节沿用 B01/B26。
- 实时事件、记录 JSON 和 run --output jsonl 是三种边界；转换集中维护，不能把 RPC 的 id/seq/jsonrpc 印到原 stdout。
- JSON 模式字段保持 session_id/status/error/result/steps/tool_calls/usage/duration_ms。
- JSONL 首条 session 及后续 type/字段保持 `agent/events.cpp` 的既有公开形状。可以改变网络分块，但不能改变可还原文本、次序和状态语义。
- 日志只记方法、身份、错误与耗时；不原样记录 credential、控制请求正文或完整配置。
- 有形的未知展示数据沿用原文本回退；未知必须处理的控制动作报错，不假装已经执行。

## 9. 进程集成完成条件

前端独立链接并能启动正式后端；前端没有 Agent/SQLite/MCP 实现依赖，后端没有 TUI依赖。
旧 CLI输出、UI 操作和所有 B 保持项经真实运行确认。
能够取消审批等待、模型请求及子组；前端退出后其后端能够收尾；没有共享进程发现与自动重连代码。
仅“socket 能连”和“能收发 JSON”不算完成协议任务。
