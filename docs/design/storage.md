# storage：SQLite 会话与审阅存储

`storage` 模块（`dagent_storage`）实现核心层定义的持久化端口，使用内嵌编译的 SQLite 管理两类独立数据库：**主会话数据库（`data/dagent.db`）**与**决策审阅数据库（`data/decision.sqlite`）**。

**职责边界**：
- 负责顺序事件追加、敏感字段脱敏、跨进程会话写锁、只读历史分页与决策阈值存储。
- **不感知业务记录结构**：事件记录的具体 JSON 字段与恢复逻辑由 `agent` 核心的 `RecordCodec` 与 `SessionRecovery` 负责，存储层将 `payload` 视为不透明 JSON（写入前递归脱敏 `api_key`、`authorization`、`token`）。

## 1. 主会话数据库设计（`dagent.db`）

### 1.1 核心表结构
```sql
CREATE TABLE sessions (
  id TEXT PRIMARY KEY,          -- UUIDv7
  cwd TEXT NOT NULL,            -- 规范化工作目录（会话归属唯一依据）
  model TEXT NOT NULL,
  title TEXT,                   -- 首条用户消息首行前 60 字符
  created INTEGER NOT NULL,
  updated INTEGER NOT NULL,
  parent_id TEXT,               -- 子 Agent 所属父会话 ID（顶层会话为空）
  agent_name TEXT               -- 子 Agent 名称
);

CREATE TABLE events (
  session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
  seq INTEGER NOT NULL,         -- 会话内严格递增的存储序号
  type TEXT NOT NULL,
  payload BLOB NOT NULL,        -- 脱敏后的 UTF-8 JSON
  PRIMARY KEY (session_id, seq)
) WITHOUT ROWID;

CREATE INDEX sessions_by_cwd ON sessions(cwd, updated DESC);
CREATE INDEX sessions_by_parent ON sessions(parent_id, created);
```

### 1.2 写入、同步与崩溃恢复语义
- **WAL 并发模式**：写连接启用 `journal_mode=WAL` 与 `synchronous=NORMAL`，父子会话各自持有独立连接并发追加事件。整轮模型与工具执行不包裹在长数据库事务内。
- **批量刷盘（Sync）**：单条事件追加不强制 fsync；在每轮对话结束（`turn_end`）、手动压缩完成及会话销毁时调用 `sync` 刷新缓存。
- **写入故障隔离（Broken 状态）**：若运行中发生序列化或磁盘写入失败，核心提交器立即进入 `broken` 状态，停止后续落盘以避免序号断层，并向界面发出一次性错误通知，但允许当前内存回合继续完成。
- **崩溃前缀恢复**：进程意外终止时，已落盘事件构成完整前缀。下次显式恢复会话时，核心扫描末尾是否有未闭合的 `user` 回合或工具调用，自动补齐「结果未知」说明与 `crashed` 结束标记后再继续。

## 2. 决策审阅数据库（`decision.sqlite`）

独立于主会话库，缺失或损坏不影响会话恢复，包含四张核心表：
- `reviews`：每次 d3 审阅的完整上下文、问句版本、各维度原始得分、阈值快照与审阅结论。
- `feedback`：人工最终审批决定与审阅记录的关联（按审批请求 ID 幂等去重）。
- `thresholds` / `fixed_thresholds`：按完整版本身份隔离的动态可调阈值与场景固定风险边界。

当且仅当审阅结论为 `approve` 或因检查未通过转人工（`failed_checks`）、审阅链中存在**唯一未达标的可调授权维度**且其原始得分落在 `[min(strict, relaxed), max(strict, relaxed)]` 可调区间内时，系统才在人工审批后于单事务内按步长微调该维度的当前阈值（批准向宽松端走 1/10，拒绝向严格端走 3/10，并递增 `revision`）。

## 3. 只读查询与历史分页

会话列表、子会话查询与历史回放均使用**只读连接**（库不存在时返回空且不建库）：
- **会话列表过滤**：严格匹配当前规范化 `cwd`，且排除带 `parent_id` 的子 Agent 会话（子会话通过 `list_children` 按父会话单独列出）。
- **游标式历史分页（`HistoryRead`）**：打开时捕获当前 `MAX(seq)` 高水位，按页（默认每页扫描 100 条记录）顺序推进并投影为 `HistoryItem`，跨页仅维护轻量级校验游标，无需在内存中重建完整 `Session`。

## 4. 写所有权（`SessionWriteLease`）

同一 `session_id` 同一时刻只允许一个可写执行者：
- 在创建或恢复可写会话前，对 `run/session-locks/<id>.lock` 获取非阻塞独占文件锁（Linux `flock`，Windows `LockFileEx`）。
- 同一进程内切换模型时直接移交现有租约，不重复加锁；锁句柄设置禁止子进程继承，随会话实例销毁或进程退出自动释放。
