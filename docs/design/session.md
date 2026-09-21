# session：SQLite 会话存储

头文件在 `src/public/session/`，实现在 `src/private/session/`，构建为 `dagent_session`。SQLite amalgamation 随
仓库放在 `src/public/lib/sqlite/`，以 `SQLITE_OMIT_LOAD_EXTENSION`、`SQLITE_DQS=0`、`SQLITE_THREADSAFE=1`
编译；SQLite 类型不出现在 session 公共接口里。

## 1. 公共接口

| 接口 | 作用 |
| --- | --- |
| `Writer::create(options, meta)` | 新建会话 |
| `Writer::resume(options, id)` | 打开已有会话并从下一个 seq 继续 |
| `Writer::append(type, payload)` | 脱敏并追加事件 |
| `Writer::sync()` | 刷新 SQLite 页缓存 |
| `list(options, cwd, limit)` | 精确按规范化 cwd 查询最近会话 |
| `list_children(options, parent_id)` | 按父会话升序列出子会话，供界面切换与按需回放 |
| `replay(options, id, callback)` | 按 seq 回放完整 payload |
| `new_id()` | 生成 UUIDv7 |

`Options::database` 由入口固定为 `<root>/dagent.db`。`redact_fields` 默认包含 `api_key`、`authorization`、
`token`；payload 在序列化和写库前递归脱敏。

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

每个连接设置 `foreign_keys=ON`、`journal_mode=WAL`、`synchronous=NORMAL` 和 5 秒 busy timeout。

- user 事件、标题、`open_turn=1` 在一个 `BEGIN IMMEDIATE` 事务内提交。
- 中间事件按 seq 追加；WAL/NORMAL 不要求每条事件单独 fsync。
- turn_end 事件与 `open_turn=0` 在同一事务内提交。

因此进程被杀时不会出现半条 JSON：已提交的事件是完整前缀，且 `open_turn` 保持 1。恢复通过事件前缀重建
Conversation，为尚未返回的工具调用补「意外中断」结果，再追加 crashed turn_end 并清零标记；之后可以继续新一轮。
写入失败会让 Recorder 进入 broken 状态，发 Notice，但当前模型回合继续。

父子会话各持独立的 sqlite3 连接（`Writer::Impl` 里有自己的 `Database`），并发写由 WAL + 5 秒 busy timeout
覆盖，不需要额外加锁。

## 4. 读取

列表执行：

```sql
SELECT id,title,model,created,updated
FROM sessions WHERE cwd=? AND (parent_id IS NULL OR parent_id='') ORDER BY updated DESC LIMIT ?;
```

子会话不进 `/resume` 与 `sessions` 列表，避免用户的列表被并行子 Agent 淹没；需要时用 `list_children` 取。

回放先确认 session 存在，再执行：

```sql
SELECT type,payload FROM events WHERE session_id=? ORDER BY seq;
```

回放中的 payload JSON 损坏会抛 `SessionError{corrupt}`；未知核心事件和 Conversation 不变式由 agent Recorder 层
校验。

## 5. 库损坏

数据库不存在时直接创建 schema。打开已有库后执行 pragma/schema 失败时，原文件重命名为
`dagent.db.corrupt-<unix-ms>`，日志记录原错误，再创建空库；不会删除损坏文件，也不尝试迁移旧 JSONL 会话。

SQLite 可能在运行期间生成同目录的 `dagent.db-wal` 与 `dagent.db-shm`，最后一个连接正常关闭并 checkpoint 后
通常只剩 `dagent.db`。备份静止安装目录时以数据库文件为会话事实来源。
