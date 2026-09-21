# 子 Agent 能力实施计划

日期：2026-09-21  
状态：未开始；按 P01 → P08 的依赖顺序实施。  
范围：DAgent 的子 Agent 派发、会话层级、并发执行、权限派生、工具集收窄、MCP 共享与界面切换。  
入口：先做 session 层级与定义加载，再落子 Agent 骨架；不得先开放并发与界面，再补权限派生。

## 1. 问题与目标

当前一个 `Agent` 对应一个会话，所有工作都挤在同一条消息历史里。大范围检索、独立的多文件实现
会把无关的工具输出堆进主上下文，触发压缩，并让主线的结论被稀释。

本次目标是引入子 Agent：由主 Agent 派发一段自足的任务，子 Agent 在自己的上下文、自己的线程、
自己的会话记录里完成它，只把最终结论交回主 Agent。

职责分工：

1. 子 Agent 是完整的 `agent::Agent` 实例，不是一个特殊的工具实现。
2. 父子之间只有四条通道：任务 prompt、最终文本、权限请求、事件流；不共享对话与授权。
3. 权限只能收窄不能放宽；能否询问用户由父 Agent 的当前模式决定。
4. 会话记录成层级：子会话独立可读，父会话保留跳转锚点。
5. 并发由父 Agent 的调度器控制，审批串行化，不改动已冻结的 TUI 框架。

成功后的主要体验：主 Agent 在检索与批量实现时上下文不再被无关输出填满；用户能在界面里切到
任意一个子 Agent 的完整记录查看它做了什么；子 Agent 的提权请求在主界面上明确标出来源。

## 2. 已核实的现状

| 部分 | 当前实现 | 本次处理 |
| --- | --- | --- |
| Agent 构成 | 一个 `Agent` 持有 Conversation / Policy / Recorder / Registry / McpHub | 允许存在多个实例，理清可共享与必须独占的部件 |
| 工具层依赖 | `tools` 不依赖 `dagent_agent`，反向依赖会成环 | 子 Agent 工具实现放在 `dagent_agent`，`tools` 只加数据结构 |
| 交互工具 | ask / exit_plan 在 tools 层只产出 Intent，执行在 dispatch 特判 | 子 Agent 不走这条路：它是执行而非询问 |
| 线程 | agent 线程 + 只读并行组临时 jthread，最多 8 个 | 子 Agent 复用组机制，另设并发上限 |
| 审批桥接 | `ApprovalDialog` 单模态 overlay，`approve()` 阻塞等 promise | 并发调用会互相覆盖 overlay 导致永久阻塞，必须加锁串行 |
| 会话存储 | 单库单层 `sessions` 表，无迁移机制 | 加 parent_id / agent_name 两列，建立 user_version 迁移 |
| 事件写入 | 每个 Writer 独立连接，WAL + busy_timeout 5s | 父子并发写安全，不额外加锁 |
| Sink 线程安全 | UI 走 `rt_.post`，headless 的 output 持 mutex | 均已满足从工作线程调用的条件 |
| MCP | `McpHub` 是 `Agent` 的值成员，析构顺序靠声明顺序保证 | 改共享句柄，子 Agent 只取快照不参与重连 |
| Registry | 只有 add / remove_prefix / find / specs | 增加按名字保留的能力，用于收窄子 Agent 工具集 |
| 界面 | `ScrollFrame` 持单个 `Scrollback`，`Transcript` 绑它的 Document | 改成多 Pane，切换展示的 Scrollback |

相关源码：

- [Agent 与一轮循环](../src/private/agent/agent.cpp)、[Agent 接口](../src/public/agent/agent.hpp)
- [调度器](../src/private/agent/dispatch.cpp)、[权限策略](../src/private/agent/permission.cpp)
- [事件与审批接口](../src/public/agent/events.hpp)、[运行输入](../src/public/agent/options.hpp)
- [会话记录](../src/private/agent/record.cpp)、[会话存储](../src/private/session/session.cpp)
- [MCP Hub](../src/private/agent/mcp_hub.cpp)、[工具注册表](../src/private/tools/registry.cpp)
- [工具接口](../src/public/tools/tools.hpp)、[工具显示数据](../src/public/tools/view.hpp)
- [界面外壳](../src/private/ui/shell.cpp)、[记录投影](../src/private/ui/transcript.cpp)、[审批对话框](../src/private/ui/approval.cpp)
- [配置加载](../src/private/app/config.cpp)

已核实的两处具体约束：

- [dispatch.cpp](../src/private/agent/dispatch.cpp) 的并行组只接纳直接放行的只读调用，组内每块最多 8 线程，
  遇到不能并行的调用先跑完挂起组再重新 prepare。子 Agent 可以复用这套语义，不需要另造调度路径。
- [approval.cpp](../src/private/ui/approval.cpp) 的 `approve()` 把 `dialog.open` post 到渲染线程后阻塞在 promise 上。
  两个线程并发调用时，后一次 `open` 覆盖前一次的 overlay，前一个 promise 不再被满足。
  这是并发子 Agent 的硬阻塞点，必须在核心侧串行化。

## 3. 用户故事

1. 作为开发者，我希望把大范围检索交给子 Agent，以免几十条 grep 输出挤占主线上下文。
2. 作为开发者，我希望子 Agent 有独立的上下文预算，不会因为主会话接近窗口上限而被压缩。
3. 作为开发者，我希望能在界面里切到某个子 Agent 的完整记录，看清它读了什么、改了什么。
4. 作为开发者，我希望子 Agent 的提权请求出现在主界面上，并明确标出是哪个子 Agent 要的。
5. 作为开发者，我希望在 plan 模式下派出的子 Agent 不能修改文件，也不会弹出任何审批。
6. 作为开发者，我希望主 Agent 的 unrestricted 不会被子 Agent 继承。
7. 作为开发者，我希望我在子 Agent 里点的“本会话允许”不会变成主会话的长期规则。
8. 作为开发者，我希望子 Agent 不能再派子 Agent，避免不可控的展开。
9. 作为开发者，我希望一次 ctrl+c 能停掉主 Agent 和全部在跑的子 Agent。
10. 作为开发者，我希望恢复会话时 task 的结果仍然可读，并能按需展开对应的子会话。
11. 作为开发者，我希望 `sessions` 列表只显示我自己开的会话，不被子会话淹没。
12. 作为开发者，我希望子 Agent 不会因为连接 MCP 而让每次派发都付出重连代价。

## 4. 子 Agent 契约

### 4.1 定义

子 Agent 由安装根下的 `home/agents/<name>.md` 定义：frontmatter 描述能力与限制，正文是它的 system prompt。

| 字段 | 含义 |
| --- | --- |
| `name` | 工具参数里的取值，必须唯一 |
| `description` | 给主模型选择用的说明，写进 task 工具的描述 |
| `model` | 可选；缺省继承父 provider |
| `tools` | 可选；缺省为父工具集减去 task / ask / exit_plan |
| `permission` | `inherit` / `read_only` / `ask`，与父模式共同决定实际权限 |
| `max_model_calls`、`max_tool_calls` | 子 Agent 自己的一轮上限，与父上限互不相关 |

### 4.2 边界

- 子 Agent 看不到父对话。任务 prompt 必须自足，这一点要写进主 Agent 的提示词。
- 子 Agent 不与用户对话：工具集里没有 ask 与 exit_plan，`Asker` 恒为空。
- 子 Agent 不能派子 Agent，见第 7 节。
- 子 Agent 的返回值是它最后一段助手文本；空文本按失败结果交回，附带原因。
- 父 `stop_token` 直接透传给子 `run_turn`，一次中断全部停止。
- 子 Agent 独立持有 `tools::Context`。子 Agent 写过的文件在父 Agent 里没有 stamp，
  父 Agent 再编辑时走 stale 检测要求重读，这是正确行为，不做“优化”。

### 4.3 权限派生

原则：子权限是父权限的子集，永不提升；能否询问用户由父 Agent 的当前模式决定，不由子 Agent 自己决定。

| 父状态 | 定义 `read_only` | 定义 `inherit` | 定义 `ask` |
| --- | --- | --- | --- |
| planning | read_only + planning，Approver 为空 | 同左 | 同左 |
| read_only | read_only，Approver 为空 | read_only，Approver 为空 | read_only，Approver 为空 |
| ask | read_only，透传 | ask，透传 | ask，透传 |
| workspace | read_only，透传 | workspace，透传 | ask，透传 |
| unrestricted | read_only，透传 | 降级为 workspace，透传 | ask，透传 |

- plan 与 read_only 下 Approver 传空。调度器已有无审批器的分支，子模型会收到明确说明并停手；
  不新增“禁止询问”的特殊机制。
- unrestricted 不继承。用户给 unrestricted 是针对自己盯着的这个会话，不是对自主运行的子 Agent 的授权。
- 会话授权双向不继承。子 Agent 新建 `Policy`，会话规则表为空；子 Agent 里批准的“本会话允许”
  只记在子 Policy 中，随子 Agent 销毁。
- 高危硬拦与用户显式拒绝在子 Agent 内同样生效，不因父已批准同类操作而跳过。

## 5. 会话层级

- `session::Meta` 增加 `parent_id`（空表示顶层）与 `agent_name`。
- `sessions` 表增加两列与 `sessions_by_parent` 索引，通过 `PRAGMA user_version` 做一次迁移；
  这是该库第一次迁移，机制要立起来，后续变更都走它，不靠捕获 duplicate column 异常。
- `list()` 只返回 `parent_id` 为空的会话，子会话不进入 `/resume` 与 `sessions` 列表。
- 新增按父会话列出子会话的查询，供界面与恢复使用。
- 父会话里那次 task 调用的显示数据带子会话 id，是父到子的唯一跳转锚点。
- 父子各自持有 Writer，写同一个库。WAL 与 busy_timeout 已覆盖并发写，`sync()` 保持每轮一次。

## 6. 线程与并发

- 一个子 Agent 对应一个线程：`create` → `run_turn` → 析构全程在该线程内完成。
- 复用调度器现有的并行组：新增一类不对路径做判定的 Intent，`parallel()` 对它在放行时返回真。
  组的 flush 语义保持不变，遇到不能并行的调用先跑完挂起组。
- 另设子 Agent 并发上限（初值 4），低于只读组的 8：每个子 Agent 都要打模型请求。
- 审批串行化：`Agent` 持一把互斥量，所有经 task 透传的审批请求先取锁再调用父 Approver。
  等待期间其他子 Agent 继续执行自己的工具，只是审批排队。
- 同一批里并行派发的子 Agent 若改同一批文件会互相覆盖，这由主模型负责，
  在 task 工具的描述里写明，不在代码里试图检测。
- Sink 无需改造：界面侧经 `rt_.post` 入队，headless 侧的输出已持 mutex。

## 7. 工具集与递归限制

- `Registry` 增加按名字保留的方法，`Agent` 构造时在注册内置工具之后按允许列表收窄。
- 子 Agent 工具集 = 定义里的 `tools`（为空则取父全集），再硬性剔除 task、ask、exit_plan。
- task 工具的描述与参数枚举在构造时按可用定义动态生成；没有可用定义或深度大于 0 时根本不注册，
  模型看不到这个工具。
- 禁止二级子 Agent 用三重保险，任何一层单独成立：深度大于 0 不注册工具、允许列表剔除、
  执行入口按深度直接返回错误结果。

## 8. MCP 拆分

- `McpHub` 从 `Agent` 的值成员改为共享句柄，入口处创建一次，父子共用；
  成员声明仍在注册表之前，保证注册表里的 MCP 工具先于 Client 析构。
- Hub 增加只读快照方法：把当前已就绪的工具合并进传入的注册表，不等待、不重连、不产生通知。
- 子 Agent 构造时取一次快照，运行循环中跳过等待与重连；MCP 的重连、等待与断线通报只由主 Agent 做。
- 子 Agent 的允许列表默认不含 MCP 工具，除非定义里显式写出。
- 本轮不改 Hub 的通知投递语义。若以后要让子 Agent 完整参与 MCP，再把“交付一次即清空”
  改为按订阅者游标，并为断线通报加按 Agent 的已报告集合。

## 9. 界面

- 工具显示数据新增一类 task 视图：子 Agent 名、任务、子会话 id、最终文本、逐条工具摘要、
  步数、调用数、耗时、是否被中断；补齐序列化与回放解析。
- 运行中的进度不新增机制：把子 Agent 的工具开始事件转成父调用的输出块，
  父记录投影已支持流式正文。
- 子 Agent 记录切换：外壳持有多个 Pane，每个 Pane 一套 Scrollback 与记录投影，
  索引 0 为主会话；`ScrollFrame` 增加切换展示对象的方法。
- 新增命令与面板列出主会话与全部 task，选中即切换；子 Pane 下输入框禁用，
  状态行与侧栏显示该 Pane 的标题与用量。
- 恢复会话时按记录还原 task 视图，不回放子会话；用户切进去时再按需回放对应子会话。
- headless 的 jsonl 输出为子事件补上子 Agent 名与父调用 id；文本模式只输出主 Agent 的最终文本。
- TUI 框架保持冻结：以上改动全部位于 `src/*/ui`，只在框架存在缺陷时才动 `src/*/tui`。

## 10. 阶段任务

| 阶段 | 内容 | 依赖 | 交付 |
| --- | --- | --- | --- |
| P01 | 会话层级与迁移 | — | Meta 新字段、user_version 迁移、列表过滤、子会话查询 |
| P02 | 子 Agent 定义加载 | — | 定义结构、frontmatter 解析、入口映射 |
| P03 | 子 Agent 骨架（串行） | P01、P02 | 可用的 task 工具、Setup 派生、权限派生、深度限制、MCP 快照 |
| P04 | 并发执行 | P03 | 新 Intent 类别、并行资格、并发上限、审批串行、请求来源字段 |
| P05 | 界面显示 | P04 | task 视图、记录投影分支、审批对话框来源标注 |
| P06 | 子会话视图 | P05 | 多 Pane、切换命令与面板、恢复时按需回放 |
| P07 | 真实运行验收 | P03–P06 | 第 11 节矩阵的真实结果与证据 |
| P08 | 文档收口 | P07 | 设计文档同步、提示词更新、完成状态与剩余限制 |

### P01：会话层级与迁移

- 范围：`src/public/session/session.hpp`、`src/private/session/session.cpp`。
- 建立 `PRAGMA user_version` 迁移机制并完成第一版迁移；旧库升级后仍可正常列出与回放。
- `list()` 过滤顶层会话；新增按父会话列出子会话。
- 验收：已有的旧库打开后可继续使用，旧会话可恢复；新字段在创建与读取路径上都生效。

### P02：子 Agent 定义加载

- 范围：`src/public/agent/options.hpp`、`src/public/app/config.hpp`、`src/private/app/config.cpp`、入口映射。
- 解析 frontmatter 与正文；名字冲突、字段类型错误、引用了不存在的模型都要给出可定位的报错。
- 定义目录不存在时按无子 Agent 处理，不报错。
- 验收：`home/agents/` 下的定义能被加载并在日志中列出；此阶段尚无人使用它，程序行为不变。

### P03：子 Agent 骨架

- 范围：新增 `src/public/agent/subagent.hpp` 与 `src/private/agent/subagent.cpp`；
  `agent/agent`、`agent/mcp_hub`、`tools/tools` 与注册表、`src/CMakeLists.txt`。
- 实现 Setup 派生、第 4.3 节权限派生、工具集收窄、深度限制、Hub 共享与快照。
- 本阶段串行执行：子 Agent 走调度器的串行路径，事件只转成父调用的输出块。
- 父 `Agent` 暴露本轮的 Sink 与 Approver 供 task 使用，轮次结束即清空。
- 验收：在 workspace 与 plan 两种模式下各真实派发一次，返回结果正确，
  plan 下子 Agent 不修改文件且不弹审批；子 Agent 的工具列表中没有 task、ask、exit_plan。

### P04：并发执行

- 范围：`tools/tools.hpp` 的 Intent 类别、`agent/permission`、`agent/dispatch`、
  `agent/events.hpp` 的审批来源字段、`agent/record`。
- 并行资格、并发上限、审批互斥、来源字段的填写与记录。
- 验收：一批内派发多个子 Agent 并发完成；其中两个同时请求审批时对话框逐个出现，
  无线程阻塞；中断一次全部停止。

### P05：界面显示

- 范围：`tools/view`、`ui/transcript`、`ui/approval`、`ui/strings`。
- task 块的折叠标题、展开内容、运行中进度；审批对话框标注来源子 Agent。
- 验收：真实 TUI 中 task 块可折叠展开，进度实时可见；子 Agent 的审批请求能看出来源。

### P06：子会话视图

- 范围：`ui/shell` 的 Pane 管理与 `ScrollFrame`、切换命令与面板、恢复路径。
- 验收：运行中与已完成的子 Agent 都能切入查看完整记录并切回；
  恢复旧会话后切入时按需回放子会话；子 Pane 下无法输入。

### P07：真实运行验收

- 按第 11 节执行；材料只放 `temp/`，只使用实际 DAgent、真实系统工具和真实模型。
- 记录原始工具调用、派生出的权限、子会话 id、步数与调用数、退出状态与实际文件结果。
- 模型改写了指定任务的案例只能算未验证。
- 验收：全部必需案例通过；环境不支持的项不能标为通过或忽略。

### P08：文档与交付

- 同步 [agent 设计](../docs/design/agent.md)（新增子 Agent 一节：分层、线程、权限派生表）、
  [session 设计](../docs/design/session.md)、[tools 设计](../docs/design/tools.md)、[ui 设计](../docs/design/ui.md)。
- 更新 `home/system.md`：何时派发、任务 prompt 必须自足、子 Agent 不能询问用户也不能再派子 Agent。
- 提供起手的两个定义：只读检索与实现类各一个。
- 以真实能力说明限制与降级行为，不把计划抄成已实现能力。

## 11. 验收矩阵与证据

### 11.1 功能与返回

| 编号 | 类别 | 必需的真实行为 |
| --- | --- | --- |
| A01 | 基本派发 | 派出只读子 Agent 完成一次检索，结论正确回到主 Agent |
| A02 | 上下文隔离 | 子 Agent 的工具输出不进入父消息历史；父只收到最终文本 |
| A03 | 独立预算 | 子 Agent 触发自己的压缩不影响父会话用量 |
| A04 | 空结果 | 子 Agent 未产出文本时父收到明确的失败结果而非空串 |
| A05 | 中断 | 运行中一次中断同时停止父与全部子 Agent，无遗留线程 |
| A06 | 上限 | 子 Agent 触到自己的调用上限时正常收尾并交回已有结论 |

### 11.2 权限

| 编号 | 场景 | 必需的真实行为 |
| --- | --- | --- |
| B01 | plan 模式 | 子 Agent 不修改文件，不出现任何审批对话框，模型收到明确说明 |
| B02 | ask 模式 | 子 Agent 的写入请求在主界面弹出，并标明来源子 Agent |
| B03 | workspace + read_only 定义 | 子 Agent 无法写入，父仍可写入 |
| B04 | unrestricted | 子 Agent 实际以 workspace 运行，不获得全访问 |
| B05 | 会话授权 | 子 Agent 内批准的会话规则不出现在父的授权列表；父的规则不下传 |
| B06 | 无审批器 | headless 下子 Agent 遇到需要审批的操作明确失败，不静默执行 |
| B07 | 并发审批 | 两个子 Agent 同时请求审批时对话框逐个出现，无阻塞与覆盖 |

### 11.3 记录与界面

| 编号 | 场景 | 必需的真实行为 |
| --- | --- | --- |
| C01 | 层级 | 子会话带父 id 与子 Agent 名，`sessions` 列表不显示子会话 |
| C02 | 迁移 | 升级前创建的旧库可继续列出、恢复与写入 |
| C03 | 并发写 | 多个子 Agent 并发写库不丢事件、不报错 |
| C04 | 回放 | 恢复父会话后 task 块内容完整；切入子会话能看到完整记录 |
| C05 | 切换 | 运行中切入子 Pane 可见实时事件，切回主会话不丢内容 |
| C06 | headless | jsonl 中子事件带子 Agent 名与父调用 id，文本模式只输出主结论 |

### 11.4 递归与边界

| 编号 | 场景 | 必需的真实行为 |
| --- | --- | --- |
| D01 | 递归 | 子 Agent 的工具列表中没有 task；即便构造被绕过也返回错误结果 |
| D02 | 交互工具 | 子 Agent 的工具列表中没有 ask 与 exit_plan |
| D03 | MCP | 派发子 Agent 不触发 MCP 重连；主 Agent 的 MCP 状态不受影响 |
| D04 | 文件状态 | 子 Agent 改过的文件，父 Agent 再编辑时被要求重读 |

### 11.5 验证方式

严格遵循 [AGENTS.md](../AGENTS.md)：

- 不新增测试源码、测试脚本、mock、演示入口、测试构建目标或模拟模型。
- 编译使用 `cmake --build --preset dev --target dagent -j2`，仅构建实际程序。
- 真实功能运行与必要材料只放在 `temp/`；不创建独立检查程序取代实际 DAgent 工作流。
- 可使用实际 CLI / PTY 与已有工具读取产生的 JSONL、SQLite 事件、退出状态与文件结果。
- 证据报告记录任务、模式、子 Agent 定义、预期、实测、会话标识与材料位置；不包含密钥。
- 文档不依赖某个临时目录长期存在；收口时保留结论与复现步骤，临时材料仅作当次证据。
- 构建成功不等于隔离正确；模型说完成也不等于子 Agent 实际执行。
- 完成必需真实运行后不反复做无目标 review。

## 12. 范围与迁移限制

本轮不做：子 Agent 之间直接通信、子 Agent 向父 Agent 追问、跨会话复用子 Agent 实例、
远程或容器化执行子 Agent、子 Agent 参与 MCP 重连与通知、多级嵌套、
按子 Agent 单独配置模型服务商凭据、为子 Agent 单独设计提示词编辑界面、无关 TUI 重构。

文件布局保持不变：hpp 仅在 `src/public/`，cpp 仅在 `src/private/`。
TUI 框架自 2026-09-18 冻结，只修相关缺陷，不扩展公开原语；界面改动落在 `src/*/ui`。

迁移顺序为“存储与定义先行、骨架串行可用、并发与界面后开”。
不得先开放并发再补审批互斥，也不得先做界面切换再补会话层级。
回退只能退回串行执行或不注册 task 工具，不能保留一个能派发但权限未派生的中间状态。

## 13. 完成标准

- [ ] P01–P08 均有状态与对应交付；受阻项明确标记未完成。
- [ ] 子 Agent 的上下文、授权与文件状态与父 Agent 相互独立。
- [ ] 权限派生表与真实运行一致，plan 与 unrestricted 两端均已验证。
- [ ] 子 Agent 的提权请求在主界面上可见且标明来源，并发审批不阻塞。
- [ ] 会话记录成层级，旧库可用，子会话可切换查看。
- [ ] 二级子 Agent 在三层保险下均不可达。
- [ ] 派发子 Agent 不引起 MCP 重连。
- [ ] 必需真实运行矩阵通过；没有新增测试代码或无关实现。
- [ ] 设计文档与提示词同步真实结果。

## 14. 参考依据

以下资料用于设计取舍，不替代本机环境的实际验收：

- [Claude Code subagents](https://docs.claude.com/en/docs/claude-code/sub-agents)：定义文件格式、工具收窄与派发边界。
- [OpenCode agents](https://opencode.ai/docs/agents/)：主从模式下的权限与模型配置分工。
- [现有 agent 设计](../docs/design/agent.md)、[现有 session 设计](../docs/design/session.md)、
  [现有 tools 设计](../docs/design/tools.md)、[现有 ui 设计](../docs/design/ui.md)：迁移起点，描述当前能力。
