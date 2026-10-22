# storage：SQLite 会话存储

头文件在 `src/public/storage/`，实现在 `src/private/storage/`，构建为 `dagent_storage`，命名空间 `dagent::storage`。
它实现核心定义的存储端口（`agent/port_journal.hpp`、`agent/port_store.hpp`、`agent/port_lease.hpp`），只依赖 agent 与 base；
记录的字段与编解码由核心 `RecordCodec` 负责，本模块把 payload 当作不透明 JSON。SQLite amalgamation 随仓库放在
`src/public/lib/sqlite/`，以 `SQLITE_OMIT_LOAD_EXTENSION`、`SQLITE_DQS=0`、`SQLITE_THREADSAFE=1` 编译；SQLite 类型不出现在对外存储接口里。

`storage/sqlite.hpp` 与 `private/storage/sqlite.cpp` 提供 `dagent::storage::detail` 内部共用的
`Statement`、`exec` 和错误抛出辅助。`storage.cpp`、`decision_store.cpp` 各自保留数据库打开方式、
schema 初始化、版本与业务校验。
`storage/platform.hpp` 声明模块内部的 `SessionFileLock` 与 `make_decision_file_private`。
会话锁原生实现位于 `storage_{posix,win}.cpp`，决策库权限操作位于 `decision_store_{posix,win}.cpp`，
CMake 分别通过 `storage_${DAGENT_PLATFORM}.cpp` 和 `decision_store_${DAGENT_PLATFORM}.cpp` 选择。
决策库仍在 SQLite 打开成功后、设置 busy timeout 前调用权限辅助；POSIX 忽略 chmod(0600) 返回值，
Windows best-effort 设置当前用户/SYSTEM/Administrators 的受保护私有 DACL。
`Statement::text` 保留原始指针绑定语义（空指针绑定 NULL）；决策库显式使用 `text_or_empty`，
将空视图绑定为 `''`。BLOB 仍直接传递原始指针与字节数。
通用 `real` 读取只做 SQLite 数值转换；决策库的 `threshold_real` 先检查列类型，
非 INTEGER/FLOAT 时仍抛出 `corrupt`，文案为 `decision threshold is not numeric`。

## 1. 公共接口

| 接口 | 作用 |
| --- | --- |
| `open_store(options)` | 返回 `agent::SessionStore`：按 seq 区间读取记录、查询高水位、打开追加写入器 |
| `SessionStore::open_writer_create(meta)` / `open_writer_resume(id)` | 返回 `agent::JournalWriter`（SQLite 实现），新建会话或从 MAX(seq)+1 续写 |
| `JournalWriter::append(record)` / `sync()` | 脱敏并追加一条记录 / 刷新页缓存；失败抛中立 `RecordError{io, not_found, corrupt}` |
| `SessionWriteLease::acquire(options, id)` | 取得同一会话的跨进程写所有权 |
| `HistoryRead::open(options, id)` | 只读历史分页（`storage/history_read.hpp`），产出核心 `HistoryItem` |
| `list(options, cwd, limit)` | 精确按规范化 cwd 查询最近的顶层会话 |
| `list_children(options, parent_id)` | 按父会话升序列出子会话，供界面切换 |
| `new_id()` | 生成 UUIDv7 |

`Options::database` 由装配固定为 `<root>/data/dagent.db`，`lock_directory` 为 `<root>/run/session-locks`。首次写入自动建立数据库父目录。`history_scan_limit` 由 `session.history_scan_limit` 提供每页扫描上限，必须为正整数。`redact_fields` 默认包含 `api_key`、`authorization`、
`token`；payload 在序列化和写库前递归脱敏，只改交给持久化的副本。存储层自身的失败为 `StorageError{io, not_found,
corrupt, invalid_state}`，经端口返回核心前转换成 `RecordError`，交给 app 查询适配时转换成 `backend::QueryError`。

## 2. Schema

```sql
CREATE TABLE sessions (
  id TEXT PRIMARY KEY,
  cwd TEXT NOT NULL,
  model TEXT NOT NULL,
  title TEXT,
  created INTEGER NOT NULL,
  updated INTEGER NOT NULL,
  parent_id TEXT,
  agent_name TEXT
);
CREATE INDEX sessions_by_cwd ON sessions(cwd, updated DESC);
CREATE INDEX sessions_by_parent ON sessions(parent_id, created);

CREATE TABLE events (
  session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
  seq INTEGER NOT NULL,
  type TEXT NOT NULL,
  payload BLOB NOT NULL,
  PRIMARY KEY (session_id, seq)
) WITHOUT ROWID;
```

时间为 Unix 毫秒。`payload` 是 UTF-8 JSON 文本的 BLOB；大工具输出直接存在同一数据库，不再有外置 blob。
web 结果使用现有 tool 记录保存 `WebView` 和该次返回的正文，不新增数据库表。网页完整缓存只在 Context 内存中，
恢复历史可显示已返回的页面片段，但不能恢复未返回的缓存内容或网络授权。
首条 user 事件写入时计算第一行标题（最多 60 个 UTF-8 字符），列表不读取事件正文。`parent_id` / `agent_name`
标识子 Agent 会话，顶层会话为空。

### schema 版本

数据库只支持 `PRAGMA user_version=1`。新库在一个事务中创建完整表与索引并写入版本；
已有库的读写入口都先检查版本。非空的版本 0 数据库及其他版本直接报 unsupported，不迁移、不改名、不重建。
已有版本 1 数据库中的额外列不影响读取；代码不再读取或维护 `open_turn` 列，新库也不创建它。

## 3. 写入与崩溃

写连接设置 `foreign_keys=ON`、`journal_mode=WAL`、`synchronous=NORMAL` 和 5 秒 busy timeout。

- 新建会话插入 sessions 行：规范化 cwd，title 为 NULL，保存初始 model 与父关系。
- user 事件、标题（首行前 60 个 UTF-8 字符）与 `updated` 在一个 `BEGIN IMMEDIATE` 事务内提交。
- turn_end 事件与 `updated` 在同一事务内提交；其他事件按 seq 追加并更新 `updated`，不另开总事务。
- `sync` 调用 `sqlite3_db_cacheflush`；WAL/NORMAL 不承诺每条事件单独 fsync。

进程被杀时已提交的事件是完整前缀。核心按 user / turn_end 记录判断回合是否闭合，为未闭合调用补「结果未知」并追加 crashed turn_end。
序列化、写入或同步失败时核心提交器进入 broken 状态、停用后续写入并只提示一次，当前回合继续；
存储层不重试旧 seq，下次续写重新读取 MAX(seq)。

父子会话各持独立的连接，并发写由 WAL + busy timeout 覆盖。整轮模型/工具执行不包在数据库事务里。

## 3.1 决策审阅库

`data/decision.sqlite` 独立保存决策审阅（与主会话库分开，缺它不影响会话恢复）：

| 表 | 内容 |
| --- | --- |
| `reviews` | 审阅 ID、approval_request_id、会话/调用/执行关联、场景、版本键、实际 state 与问句、响应分数、阈值快照、结果、固定理由、耗时 |
| `feedback` | 最终审批请求 ID、关联审阅 ID、最终来源与人工决定；主键去重，重启后不重放 |
| `thresholds` | 版本键 + 场景/维度/方向 + 严格端/宽松端/当前值与 revision |
| `fixed_thresholds` | `<scene>.risk_low` / `<scene>.risk_high` / `<scene>.unknown_authorize`（0=转后续审批，1=授权检查），以及 `injection_limit`；旧 `net_change_*` 仍可读取历史配置 |

风险优先问句版本为 `risk-first-v2`，不会继承旧版本 `1` 的阈值。`script_basic` 与
`script_mixed_basic` 分开存储，修改脚本共用 `script_diff`。新增维度 `needs_authorization`、
`evidence_sufficient`、`manual_only`、`authorization` 保留在实际请求与响应记录中；风险分流的
固定边界和灰区策略也进入每条审阅的阈值快照。任何必需配置缺失时，该场景转后续审批。

`finish()` 在一个事务内回填人工决定：只在恰好一个可调整检查不达标、分数处于两端之间时，
批准向宽松端走 1 步、拒绝向严格端走 3 步（步长为两端之差的 1/10），并限制在端点内；
多个失败检查、fixed 阈值、modify/unknown 与缺有效标定不参与调整。审阅链中任一项不可用、证据失败或缺失时也不归因更新；没有人工渠道、取消及撤权不算用户最终决定。
风险分流、证据灰区和不可代批判定不进行单维反馈调整；授权阶段的 authorization、direction、privacy
仍按上述唯一失败检查规则调整。严格端等于宽松端表示固定候选，不更新数值或 revision。

`DecisionStoreOptions.initial_profile` 接收启动配置快照，在单事务内只补齐缺失的键。
已有 adaptive 的 pass/strict/relaxed 与 fixed 值须匹配，current/revision 保留；冲突要求采用新 profile 名。
读取、导入及反馈更新共享数值校验：拒绝 NULL/文本、非有限或超出 [0,1] 的值、错误比较方向、
逆序上下界及区间外 current。非法或不完整的配置不会生成自动批准。

## 4. 写所有权

同一 session_id 同时只允许一个可写执行者。`SessionWriteLease` 对安装根下 `run/session-locks/<id>.lock` 加 `flock`：

- 在创建或恢复可写会话前取得；被其他进程持有时报 `session … is already in use by another process`。
- 同一进程内按路径复用同一个句柄，同会话切模型不会再次加锁；FD 为 CLOEXEC，工具子进程不继承。
- 锁随所有者析构或进程退出释放；锁文件不 unlink，也不根据 PID 猜测或杀死其他进程。
- 只读查询不取锁。不同会话可以由不同后端同时写入。

Windows SessionFileLock 使用不继承的同步文件句柄，在 [0,1) 字节范围执行
LockFileEx(EXCLUSIVE|FAIL_IMMEDIATELY)，不等待、不重试；锁文件创建时带私有 DACL，打开期间禁止共享删除。
句柄由最后一个共享 State 持有，析构 UnlockFileEx 再 CloseHandle；同进程复用、目录创建与注册时机仍在共享层。
文件打开失败与锁争用仍报告 StorageError(io)，不借助命名 mutex 或 PID 文件回收。

下面的 open/flock 步骤描述 POSIX 实现。

`storage.cpp` 保留注册表与互斥锁：键仍是未经额外规范化的 `path.string()`，值仍为 `weak_ptr<void>`。
查到活跃 State 时只创建租约包装并共享 State，不创建目录、打开文件或重新加锁；过期条目先删除。
新锁仍在同一互斥范围内创建父目录、分配共享 State、保存 session_id，再构造不透明 `SessionFileLock`。
其 Impl 在 open 前分配；open 使用 `O_RDWR | O_CREAT | O_CLOEXEC`、`0600`，随后一次 `flock(LOCK_EX | LOCK_NB)`，
不重试 EINTR。open 失败保留原错误文本；flock 失败先 close、置 -1，再报会话占用错误。
取得锁后才登记 State 并返回租约。最后一个共享 State 销毁时，其 SessionFileLock 先 `LOCK_UN` 再 close，
注册表不持有强引用；登记或租约分配失败时也由该 State 的析构释放锁。

## 5. 读取

只读路径（列表、子会话、`SessionStore::read_records`、历史分页）使用只读连接：不执行 schema 初始化、不修复、不重命名损坏库。
数据库不存在时列表返回空且不创建文件；打开失败直接报告查询错误。

```sql
SELECT id,title,model,created,updated
FROM sessions WHERE cwd=? AND (parent_id IS NULL OR parent_id='') ORDER BY updated DESC LIMIT ?;
```

子会话不进 `/resume` 与 `sessions` 列表，需要时用 `list_children` 取。

`HistoryRead` 打开时捕获会话元信息与 MAX(seq) 高水位，之后按 seq 推进：每页扫描量不超过 `history_scan_limit`，无显示项的一页也推进，
Storage 内部顺序推进 next_seq，Backend 为每一页分配独立的查询游标并校验会话归属。页之间只保存核心 `HistoryCursor` 的验证元数据（ordinal、开放调用、裁剪目标），不构造 Conversation。
读完、关闭或失败后再读返回 `invalid_state`。记录损坏（payload JSON 或核心字段非法）报 corrupt。

## 6. 库损坏

读写入口遇到 pragma/schema 错误或不支持的版本时直接报错；不改名、不删除、不重建已有数据库，也不迁移其他会话格式。

SQLite 可能在运行期间生成同目录的 `dagent.db-wal` 与 `dagent.db-shm`。备份活动安装根时使用 SQLite 一致备份，
不要只复制主数据库文件。
