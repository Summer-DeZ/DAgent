# storage：SQLite 会话存储

头文件在 `src/public/storage/`，实现在 `src/private/storage/`，构建为 `dagent_storage`，命名空间 `dagent::storage`。
它实现核心定义的存储端口（`agent/port_journal.hpp`、`agent/port_store.hpp`、`agent/port_lease.hpp`），只依赖 agent 与 base；
记录的字段与编解码由核心 `RecordCodec` 负责，本模块把 payload 当作不透明 JSON。SQLite amalgamation 随仓库放在
`src/public/lib/sqlite/`，以 `SQLITE_OMIT_LOAD_EXTENSION`、`SQLITE_DQS=0`、`SQLITE_THREADSAFE=1` 编译；SQLite 类型不出现在公共接口里。

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

`Options::database` 由装配固定为 `<root>/dagent.db`。`redact_fields` 默认包含 `api_key`、`authorization`、
`token`；payload 在序列化和写库前递归脱敏，只改交给持久化的副本。存储层自身的失败为 `StorageError{io, not_found,
corrupt, invalid_state}`，经端口返回核心前转换成 `RecordError`，交给 app 查询适配时转换成 `runtime::QueryError`。

## 2. Schema

```sql
CREATE TABLE sessions (
  id TEXT PRIMARY KEY,
  cwd TEXT NOT NULL,
  model TEXT NOT NULL,
  title TEXT,
  created INTEGER NOT NULL,
  updated INTEGER NOT NULL,
  open_turn INTEGER NOT NULL DEFAULT 0,
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
首条 user 事件写入时计算第一行标题（最多 60 个 UTF-8 字符），列表不读取事件正文。`parent_id` / `agent_name`
标识子 Agent 会话，顶层会话为空。

### schema 迁移

`PRAGMA user_version` 是该库的迁移版本号。`initialize()` 在 `CREATE TABLE IF NOT EXISTS` 之后按版本补齐旧库：

```cpp
if (user_version() < 1) {
    if (!has_column("sessions", "parent_id"))  exec("ALTER TABLE sessions ADD COLUMN parent_id TEXT;");
    if (!has_column("sessions", "agent_name")) exec("ALTER TABLE sessions ADD COLUMN agent_name TEXT;");
    exec("CREATE INDEX IF NOT EXISTS sessions_by_parent ON sessions(parent_id, created);");
    exec("PRAGMA user_version=1;");
}
```

后续所有 schema 变更都递增版本、在同一个块里就地迁移。不能用「捕获 duplicate column 异常」代替：构造函数里的
`initialize()` 抛异常会把数据库当作损坏文件改名备份，那样用户会丢掉全部历史会话。

## 3. 写入与崩溃

写连接设置 `foreign_keys=ON`、`journal_mode=WAL`、`synchronous=NORMAL` 和 5 秒 busy timeout。

- 新建会话插入 sessions 行：规范化 cwd，title 为 NULL，open_turn=0，保存初始 model 与父关系。
- user 事件、标题（首行前 60 个 UTF-8 字符）、`updated`、`open_turn=1` 在一个 `BEGIN IMMEDIATE` 事务内提交。
- turn_end 事件与 `open_turn=0` 在同一事务内提交；其他事件按 seq 追加并更新 `updated`，不另开总事务。
- `sync` 调用 `sqlite3_db_cacheflush`；WAL/NORMAL 不承诺每条事件单独 fsync。

进程被杀时已提交的事件是完整前缀，`open_turn` 保持 1。核心的显式恢复据此为未闭合调用补「结果未知」并追加 crashed turn_end。
写入失败时核心提交器进入 broken 状态、只提示一次，当前回合继续（B23）；存储层不重试旧 seq，下次续写重新读取 MAX(seq)。

父子会话各持独立的连接，并发写由 WAL + busy timeout 覆盖。整轮模型/工具执行不包在数据库事务里。

## 4. 写所有权

同一 session_id 同时只允许一个可写执行者。`SessionWriteLease` 对安装根下 `.runtime/session-locks/<id>.lock` 加 `flock`：

- 在创建或恢复可写会话前取得；被其他进程持有时报 `session … is already in use by another process`。
- 同一进程内按路径复用同一个句柄，同会话切模型不会再次加锁；FD 为 CLOEXEC，工具子进程不继承。
- 锁随所有者析构或进程退出释放；锁文件不 unlink，也不根据 PID 猜测或杀死其他进程。
- 只读查询不取锁。不同会话可以由不同后端同时写入。

## 5. 读取

只读路径（列表、子会话、`SessionStore::read_records`、历史分页）使用只读连接：不执行 schema 初始化、不修复、不重命名损坏库。
数据库不存在时列表返回空且不创建文件；打开失败直接报告查询错误。

```sql
SELECT id,title,model,created,updated
FROM sessions WHERE cwd=? AND (parent_id IS NULL OR parent_id='') ORDER BY updated DESC LIMIT ?;
```

子会话不进 `/resume` 与 `sessions` 列表，需要时用 `list_children` 取。

`HistoryRead` 打开时捕获会话元信息与 MAX(seq) 高水位，之后按 seq 推进：每页最多扫描 100 条记录，无显示项的一页也推进，
游标对前端不透明。页之间只保存核心 `HistoryCursor` 的验证元数据（ordinal、开放调用、裁剪目标），不构造 Conversation。
读完、关闭或失败后再读返回 `invalid_state`。记录损坏（payload JSON 或核心字段非法）报 corrupt。

## 6. 库损坏

写入口打开数据库时若 pragma/schema 失败，原文件重命名为 `dagent.db.corrupt-<unix-ms>`，日志记录原错误，再创建空库；
不会删除损坏文件，也不尝试迁移旧 JSONL 会话。只读查询遇到同样的问题只报错，不改名。

SQLite 可能在运行期间生成同目录的 `dagent.db-wal` 与 `dagent.db-shm`。备份活动安装根时使用 SQLite 一致备份，
不要只复制主数据库文件。
