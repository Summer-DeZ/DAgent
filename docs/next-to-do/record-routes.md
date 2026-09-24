# 记录路线、字段与恢复契约

状态：已实施（R01–R14 真实验收，见 [执行记录](implementation-tasks.md#7-执行记录)）。本文是对 [状态机](execution-state-machines.md) 中“统一提交”的逐项定义，
也是 R05/R06/R07/R10 的必读输入。功能边界仍以 [B保持清单](frontend-backend-migration.md#4-功能保持清单) 为准。

证据基线：`src/private/agent/record.cpp`、`agent.cpp`、`dispatch.cpp`、`compaction.cpp`、
`src/private/session/session.cpp`、`src/private/agent/events.cpp`。
字段与行为以这些已核对的写入/读取路径为依据；不从 UI 名称猜测数据库格式。

## 1. 三种序号和四条出口

| 名称 | 所属位置 | 递增规则 |
| --- | --- | --- |
| record_seq | SQLite events.seq | 每个会话从 0 开始；每条成功追加的记录递增；resume 从 MAX(seq)+1 开始 |
| message_ordinal | user/assistant/tool payload.n | 仅这三种消息递增；system/permission/压缩不消耗；压缩不重编号历史 |
| event_seq | 私有连接通知的 seq | 本次后端的发送序列，与数据库序号无换算关系 |
| invocation_id | 新内部/IPC身份 | 区分本轮、步骤和动作；不替换原持久 call_id |
| model_call_id | 模型返回的调用 ID | 原样用于 assistant.tool_calls、tool.call_id 和兼容 JSONL |

摘要前缀在 Conversation 中使用 ordinal=-1，是内存合成条目；不要另写一条 n=-1 的 user 记录。
Run、input、interaction 的内部身份不新增数据库字段或表。

四条出口分别为：

1. SessionCommitter → RecordCodec → JournalWriter → SQLite：持久历史。
2. 核心进度/完成通知 → Backend adapter → Protocol Event：当前 UI/CLI 实时信息。
3. ReadOnlyStore → RecordCodec → HistoryProjector → HistoryItem：只读历史显示。
4. ReadOnlyStore → RecordCodec → SessionRecovery → 恢复状态 → 显式恢复提交：继续执行。

3 不调用 4。CLI 的 LegacyOutputCodec 从 2 恢复既有 JSONL 外观，不能把 1 的 payload 或 RPC信封直接打印。

## 2. 全部持久记录类型与字段

下表字段为写入形状；历史读取的可选字段兼容见 §6。类型缩写：str=字符串、i64=整数、bool=布尔、obj=对象、array=数组。
JSON 对象/数组在脱敏后仍必须保持原容器类型。

| 类型 | payload 必需写入字段 | 版本/说明 |
| --- | --- | --- |
| system | schema:i64、text:str、model:str | schema=1；恢复/切模型也会再次追加 |
| user | n:i64、text:str | 第一条有效 user 决定列表 title |
| assistant | n:i64、content:str、reasoning:str、reasoning_signature:str、tool_calls:array、finish:str；有 usage 时增加 usage:obj | tool_calls 每项 id/name/arguments 都是字符串；arguments 保留原 JSON 字符串 |
| tool_started | schema:i64、id/name/summary:str、sandbox/backend/grant_source:str、analysis_version:i64、network/local_sockets/private_tmp/protect_sensitive_names:bool、readable/writable/protected_read/protected_write/network_targets:array[str] | schema=1；写入对象不含实时通知的 type 字段 |
| tool | n:i64、call_id/name/summary/text:str、is_error/interrupted:bool、view:obj | view 为原 tools::to_json 的 kind 结构 |
| permission | schema:i64、call_id/answer/rule/cwd/mode:str、network/partially_executed:bool、requests:array | schema=2；requests 项为 kind/target/reason 字符串 |
| permission_revoked | schema:i64、id:str | schema=1 |
| turn_end | status/error:str、steps/tool_calls:i64、usage:obj | status 为现有结果；恢复闭合还使用 crashed |
| prune | ordinals:array[i64] | 必须指向当时保留的 tool 消息 |
| compaction | keep_from:i64、summary:str | summary 为空表示原有丢弃前缀退化 |

usage 固定字段为 prompt/completion/cached 整数。
permission.answer 保持 allow / allow_session / deny / deny_with_feedback。
permission.requests.kind 保持当前 dynamic_command/read_path/write_path/network/sensitive_read/protected_write/host_access。

不新增 plan_changed、mode_changed、interaction_answer 或 run_finished 数据库记录类型。
Plan 的持久来源是 todo 对应 tool.view；规划确认的持久来源是 exit_plan 的工具调用/结果。
实时同名事件不意味着数据库存在同名记录。

## 3. SQLite 元信息与事务

sessions 继续保留 id、cwd、model、title、created、updated、open_turn、parent_id、agent_name。

| 操作 | 元信息行为 | 事务边界 |
| --- | --- | --- |
| Writer.create | 插入会话，规范化 cwd，title=NULL、open_turn=0，保存初始 model/父关系和毫秒时间 | 原 INSERT |
| user append | 写事件，title=COALESCE(title,首行前60个UTF-8字符)，updated=当前时间，open_turn=1 | 与事件在同一个原有事务内 |
| turn_end append | 写事件，updated=当前时间，open_turn=0 | 与事件在同一个原有事务内 |
| 其他 append | 写事件，更新 updated，不改变 open_turn | 保持原先提交方式；不补一个新总事务 |
| system append | 按其他 append 处理；sessions.model 不随它同步修改 | 最近模型从 system 记录解码，列表元字段语义保持 |
| Writer.resume | 读取原 Meta 和下一 record_seq | 不创建新 session_id |
| sync | 保留原 sqlite3_db_cacheflush 语义 | 不宣称每条事件逐条 fsync 或断电绝对不丢失 |

脱敏在 storage 的 append 中、payload.dump 与写库之前发生；只修改交给持久化的副本，不把运行中的工具参数改成脱敏值。
父子各用自己的 Writer 和 SQLite 连接，SQLite WAL 与现有 busy timeout 保持。
禁止把整轮模型/工具执行包在持有数据库写锁的事务里。

## 4. 逐入口路线表

C 表示 SessionCommitter，J 表示 JournalWriter；“通知”指既有实时语义，不自动意味着新增旧 JSONL 字段。

| 路线 | 入口/条件 | 内存与记录顺序 | 通知/返回 |
| --- | --- | --- | --- |
| L01 | 新建顶层会话 | 准备 prompt/配置 → Writer.create → 组装 Session → C 写 system | 返回会话状态，不伪造 TurnStarted |
| L02 | 创建子会话 | 与 L01相同，Meta增加 parent_id/agent_name | 子流从实际子 Run事件开始，保留父调用关联 |
| L03 | 普通输入出队 | 分配 Run → 合法UTF-8输入 → Conversation加user取n → J写user → 检查broken | TurnStarted，随后模型步骤 |
| L04 | 模型流增量 | 不修改正式 Conversation，不落盘每个chunk | TextDelta/ReasoningDelta/ToolPending/Retrying/StreamReset |
| L05 | 完整模型回复 | Conversation加assistant取n → J写assistant → 检查broken | 正文已有流通知；不再重复发送一次完整正文作为新增量 |
| L06 | 普通串行动作开始 | J写tool_started → 检查broken → 发布开始 → execute | ToolStarted、执行过程ToolOutput |
| L07 | 并行组开始 | 在父执行线程依原顺序写并发布组内tool_started，再按原宽度分块执行 | 输出可交错，结果进各自槽位 |
| L08 | 正常工具/准备错误/未知工具/拒绝/超限结果 | 等待槽位前缀可提交 → Conversation加tool取n → J写tool → 检查broken | ToolFinished；未执行者不补普通tool_started |
| L09 | 审批有回答且未取消 | J写permission → 检查broken → 按原分支记会话授权或生成拒绝结果 → L06/L08 | 等待UI只负责回答；最终Grant由Policy生成 |
| L10 | 审批回答前已取消 | 不追加permission，不执行调用 → 中断工具结果进入L08 | 原中断语义 |
| L11 | 撤销现有授权 | Policy.revoke成功 → J写permission_revoked → sync | 返回removed；不存在时不追加记录 |
| L12 | ask / exit_plan | 依原行为实时发布ToolStarted，但不写普通tool_started记录；执行现有问答/模式逻辑；结果进L08 | 问答交互；exit_plan成功时先ModeChanged，再提交工具结果 |
| L13 | todo | 正常准备/调度；到原序提交点更新WorkPlan，并在同一个tool记录中写原TodoView | ToolFinished的计划数据；无额外plan记录 |
| L14 | task完成 | 子会话各自L02–L18；汇总原TaskView；父调用结果走L08 | 子流与父最终结果不重复落进父会话 |
| L15 | 模型请求中断且有partial正文 | 只构造现有“正文+中断标记”assistant → 写assistant | 不保存未完成tool_calls，也不把已有流正文再追加一遍 |
| L16 | 压缩提交 | 最终检查stop → 内存安装pending Conversation → 有pruned时写prune → 有keep_from时写compaction | 原Notice/Compacted/ContextUpdate；不是每条记录都有对应显示事件 |
| L17 | 正常结束前仍有open_calls | 按原finish填充中断tool结果到内存和记录 | 保留当前finish路径不额外发ToolFinished的语义；与恢复填充区别见L20 |
| L18 | turn结束 | L17 → 原broken检查 → J写turn_end → sync → 必要MCP通知 → Run一次性完成 | 唯一TurnEnded，再由Controller按B06 drain |
| L19 | 手动compact | 使用L16；捕获原模型错误/中断 → sync | Context/Notice与内部OperationFinished；不写user或turn_end |
| L20 | 显式恢复未闭合会话 | 纯解码/恢复校验 → 为open_calls写未知结果tool → 写crashed turn_end → sync → 写新system | 返回新历史高水位；旧式恢复ToolFinished/TurnEnded转历史展示，不能启动队列 |
| L21 | 切模型 | 空闲冻结 → 读取持久状态并准备候选 → 同session lease内按恢复语义写必要闭合/system → 替换内部状态 | 当前模型标签/Context/Notice；不把全部历史重新打印成实时流 |
| L22 | 只读历史/列表 | 只读连接读取 Meta/Record，高水位分页，使用Decoder/Projector | 返回HistoryItem/列表；零写入、零Agent/MCP构造 |
| L23 | 会话/后端销毁 | 请求取消/等待组结束 → 记录sync → 工具和连接清理 → 释放写lease | 无新的业务user/assistant；关闭通知不冒充TurnEnded |

L12 来自当前 dispatch 的显式交互分支。此次重构不能为了“所有动作统一”而给 ask/exit_plan补出原来没有的tool_started持久记录。
L17 来自当前 finish 的收尾路径，L20 来自当前 resume；两者记录相似但通知行为不同，必须在提交原因中区分。

## 5. 提交器的精确边界

统一提交是“一个对象负责相应路线”，不是“全部变化使用一条固定顺序”或“整轮数据库事务”。
固定定义以下提交原因，调用者不能随意传一个 emit=true/false 来猜显示策略：

| 提交原因 | 允许的调用者 | 语义 |
| --- | --- | --- |
| live_message | TurnRunner / ActionDispatcher | L03/L05/L08：追加内存、尝试写记录、按原事件规则通知 |
| execution_audit | OrdinaryActionExecutor / Policy流程 | L06/L07/L09/L11：审计顺序以实际分支为准 |
| control_result | ControlActionExecutor 经槽位提交 | L12/L13：按固定控制类型映射，不添加新记录类型 |
| partial_response | TurnRunner取消分支 | L15：只保存可接受的部分正文 |
| compact_commit | ContextManager | L16：内存候选一次安装，记录仍为原prune/compaction顺序 |
| turn_repair | TurnRunner.finish | L17：补内存/记录，不增加原CLI实时事件 |
| recovery_repair | SessionController.resume | L20：补记录供完整历史显示，不送入当前Run控制流 |
| lifecycle_record | create/resume/finish/close | system/turn_end/sync，字段由RecordCodec固定 |

禁止调用者自行完成“Conversation.add + writer.append + sink”三连。记录编码只有RecordCodec一份。
WorkPlan更新与todo工具结果在一个提交操作中完成，不能分两次写出重复tool记录。

## 6. 恢复与只读投影的逐类型规则

| 记录 | SessionRecovery | HistoryProjector |
| --- | --- | --- |
| system | 校验schema/text；更新最近model信息；真正运行prompt随后按当前环境重建 | 更新历史模型标签；不执行prompt |
| user | 按n恢复user，标open_turn | 一个用户条目 |
| assistant | 按n恢复正文/思考/签名/调用；保留协议关系 | 一个assistant历史条目，含正文与思考；无StepStarted伪造 |
| tool_started | 校验授权字段，保留必要审计信息；不能据此重放工具 | 原开始事实/卡片信息 |
| tool | 按n恢复调用结果和summary；todo同时更新WorkPlan | 原工具卡片/结果；todo更新显示计划，task提供历史子会话锚点 |
| permission | 校验原必需字段，但不恢复临时授权 | 不弹审批、不生成待回答交互 |
| permission_revoked | 校验schema/id，不复活或再次撤销运行中的规则 | 不执行任何控制操作 |
| prune | 校验工具ordinal，应用原占位裁剪 | 不删除原历史正文/工具卡片 |
| compaction | 校验安全切点，摘要或丢弃前缀，验证消息约束 | 不把上下文摘要当作新用户消息替换可见历史 |
| turn_end | open_turn=false；crashed按原规则映射failed | 历史结束条目，不触发当前Controller drain |

兼容规则固定：system.model、assistant.reasoning_signature/usage 可按当前读取逻辑缺失；
tool_started.protect_sensitive_names 缺失时为false；旧permission仅要求原call_id/answer/rule/network读取字段，不能因为当前写schema=2就拒绝旧形状。
未知view.kind保持原monostate/文本回退；未知核心type或非法必须字段保持corrupt错误。
已允许的额外字段继续忽略，不把严格全字段白名单当作“顺手加强校验”。

Conversation配对、ordinal递增、prune目标和安全切点的规则抽成可共享的纯状态规则。
历史读取使用只保存角色/ordinal/开放调用等必要元数据的验证游标，不构造完整Conversation、模型请求、token估算器或可执行Session。
不能为了分页只校验当前页，丢掉前后页的顺序和配对约束；也不重新手写一套不同的校验算法。

## 7. 分页读取的完整契约

ReadOnlyStore 的 read_page 输入为 session_id、固定upper_seq、next_seq、limit；按真实record_seq读取，不按显示项数计算游标。
首次session.history捕获Meta和MAX(seq)作为高水位，建立一个仅查询用HistoryRead对象。
HistoryRead拥有查询ID、下一record_seq、Record验证游标和Projector状态；不拥有Writer或写lease。

- 响应cursor为后端不透明查询游标，客户端只能原样回传；它在本次连接内有效。
- 每页最多扫描100条记录；无显示项的一页也必须推进next_seq，不能原地重试。
- 一个assistant记录产生一个含正文/思考的HistoryItem，记录和显示项不是一一计数关系。
- 每次读取使用只读连接/短查询，不在用户浏览期间持有长数据库事务；记录只追加且upper_seq固定，后续追加不进入此查询。
- 最后一页释放HistoryRead；前端关闭/切换对应页面时发送session.history_close释放未读完对象；连接关闭统一释放。
- 对已释放cursor返回invalid_state，不自动恢复执行或重新开始一轮模型。
- 顶层恢复完成后使用新的高水位取得完整历史，包含刚补的crashed工具/结束记录；此前预校验结果不直接当作最终历史发两遍。
- 切模型保留当前Transcript，只更新公开状态；不请求并追加一整份历史以免重复正文。

只读存储连接必须不执行initialize/schema修复/损坏库重命名：query打开失败直接报告查询错误。
不存在数据库时列表返回空；不存在的session历史返回not_found。写入口继续执行现有数据库初始化/恢复政策。
这不改变记录格式，也不增加历史修复功能。

## 8. 恢复、写锁和候选替换

### 8.1 普通显式恢复

1. 解析目标ID，取得目标SessionWriteLease；失败返回session_in_use。
2. 用纯恢复读取全部记录，校验开放调用的可闭合性；失败释放候选资源，旧当前会话不变。
3. 准备prompt、模型和工具环境；这些步骤不追加候选system。
4. 打开resume Writer，以MAX(seq)+1为起点；完成L20修补，再追加新system。
5. 安装候选Session，更新generation，随后读取修补后的历史高水位。
6. 旧不同会话对象收尾并释放旧lease；候选成为唯一活跃写入者。

### 8.2 同一session切模型

同一session的写lease由SessionController持有，寿命跨模型替换；禁止为候选再次flock同一文件。
候选模型/工具环境和恢复状态在旧Session空闲时准备，先不写库。
准备完成才进入提交点：旧提交器不再追加；新Writer从当时MAX(seq)+1继续，执行必要恢复/system记录。
可以短暂同时存在两个SQLite连接，但只允许新提交器追加；旧提交器只sync/销毁。
候选Writer打开失败时旧Session仍可保留；进入已知写入broken后仍按B23安装候选并告知降级，不伪造磁盘回滚。

“替换原子”仅指内存当前会话/公开状态的切换，不声称可回滚已写入的历史记录。
WorkPlan显示按B14保留旧当前值，不能因候选只读到了较早的持久todo而清空已有计划。

## 9. 写入失败与崩溃界限

- SessionCommitter 是broken/error的唯一所有者。第一次append/sync分类错误后停用后续记录写入，内存和当前回合按B23继续。
- 保留当前check_broken的通知触发点，不另造全局“失败即中断”策略。sync发生在末尾检查之后时，不能承诺当轮必然发出新的Notice。
- 非user/turn_end记录的INSERT与updated更新未必在同一事务；若后者失败，前者可能已经写入。不得用旧seq重试；下次resume重新读MAX(seq)。
- L16是内存一次提交，不是prune+compaction两个记录的新增原子事务；若只写入前者就失败，恢复以实际持久前缀为准。
- 进程在外部副作用发生后、tool结果落盘前崩溃，恢复只能报告未知结果；ToolStarted不能证明是否已完成副作用。
- 记录流与实时流在broken时允许不同；UI显示“已完成”不代表记录可靠保存，已有broken提示语义必须保留。
- 不根据残余进度日志、历史View、RPC响应或模型最终文字推断可自动重做工具。

## 10. 逐路线验收追踪

| 路线 | 实施任务 | 既有真实场景 |
| --- | --- | --- |
| L01–L05 | R05/R06/R07 | V03/V06/V07/V11 |
| L06–L11 | R04/R05 | V03/V05/V07/V15 |
| L12–L14 | R04/R07/R08 | V08/V12/V13 |
| L15–L19 | R05/R06 | V07/V09/V15/V16 |
| L20–L22 | R06/R07/R11 | V06/V10/V11/V18/V19 |
| L23 | R12/R13 | V02/V17/V18/V20 |

从实际数据库和正式输出核对记录类型、seq/n关系及副作用；不构造伪记录或新增检测程序。
R05/R06收口时逐行填写L覆盖情况。任何路线尚需选择落盘位置、写入者或回放副作用，都不能把对应任务标为可直接交接。
