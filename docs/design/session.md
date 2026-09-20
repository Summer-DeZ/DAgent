# session：会话存储

把会话事件只追加地落盘的模块。头文件在 `src/public/session/`，实现在 `src/private/session/`，构建为
静态库 `dagent_session`，命名空间 `dagent::session`。只依赖 base，没有第三方库——JSONL 加 nlohmann，
可以直接用 `jq`、`grep` 排查问题。

**事件里具体放什么由核心决定**：本模块只管外层信封（类型、时间戳、序号）和存取，`payload` 对它来说是
不透明的 JSON。核心的 payload 格式、压缩回放、崩溃闭合与恢复入口见
[agent §10](agent.md#10-会话记录与恢复)。

只有一个错误类型：

```cpp
class SessionError : public std::runtime_error {
public:
    enum class Kind { io, not_found, corrupt };
};
```

---

## 1. 概述

| 能力 | 主要接口 |
| --- | --- |
| 创建 / 恢复写入 | `Writer::create`、`Writer::resume`、`Writer::append`、`Writer::sync` |
| 列出会话 | `list`（按更新时间倒序，可按项目过滤） |
| 回放 | `replay`（blob 引用还原成原文） |
| 会话 ID | `new_id`（UUIDv7） |

---

## 2. 存储布局

```
$XDG_DATA_HOME/dagent/sessions/（默认 ~/.local/share/dagent/sessions，Options::directory 可覆盖）
└── 2026/09/18/
    └── 01a0b514-2b51-7102-94a4-c94eab6cd546/   会话 ID 是 UUIDv7：文件名字典序 == 创建时间顺序
        ├── events.jsonl                         0644；所在会话目录 0700
        └── blobs/000001.txt                     超过 max_inline_payload_bytes 的字符串值
```

会话目录创建后立即 `chmod 0700`（`fs::perms::owner_all`）：会话里可能含用户输入和工具输出，不假设内容
无害。目录按天分层，同一天创建的会话都落在同一个 `YYYY/MM/DD/` 下。

`events.jsonl` 第一行永远是 `type=meta` 的信封：

```json
{"type":"meta","seq":0,"ts":"2026-09-18T10:00:00.123Z","v":1,"id":"…","cwd":"…","git_root":"…","model":"…","dagent_version":"0.1.0"}
{"type":"<核心定义>","seq":1,"ts":"…","payload":{…}}
{"type":"<核心定义>","seq":2,"ts":"…","payload":{"output":{"$blob":"blobs/000001.txt","bytes":183422}}}
```

会话属于哪个项目，由 meta 行的 `cwd` 和 `git_root` 判断（`list` 按项目过滤时用）。

---

## 3. Writer：写入

- **`create`**：`meta.id` 为空时生成新的 UUIDv7；按当天日期建目录（含 `blobs/` 子目录）并设权限；用
  `O_WRONLY | O_CREAT | O_APPEND` 打开 `events.jsonl`，立即写 meta 行。
- **`append`**：组装 `{type, seq, ts}`，`record_payloads=true` 时按顺序**先 `base::redact` 脱敏，
  再把超过 `max_inline_payload_bytes` 的字符串值转成 blob 引用**，脱敏结果不会意外落进 blob 文件；
  最后整行 `dump() + '\n'` 一次 `write()` 写出。`record_payloads=false` 时只写信封，没有 `payload` 字段，
  `seq` 仍然连续递增。
- **每行一次 `write()`，文件 `O_APPEND` 打开，只有一个写入者**，不需要加文件锁——这个约束由调用方（核心，
  单个 agent 线程）保证，模块自己不做并发检查。
- **`sync`**：`fsync` 当前 fd，调用节奏由核心决定（比如每轮结束调一次），不是每行都刷盘。
- blob 文件本身：先整份 `write` 完 `fsync`，`close` 之后才把引用它的那一行写进 `events.jsonl`——保证任何
  时刻看到 blob 引用，对应的 blob 文件已经落盘完整（WAL 顺序）。

---

## 4. 崩溃恢复

`Writer::resume` 打开已有会话继续写之前，先做恢复：

1. `recover_tail` 从文件末尾往前找**最后一条完整合法的 JSON 行**：按 64 KiB 窗口逐步往前扩展读取，
   跳过尾部所有解析失败或不完整的行（不止处理一行，只要靠近末尾），直到找到能 `json::parse` 成功的
   对象为止。找到的行如果缺结尾换行符，记下来（`needs_newline`）。
2. 找到的有效字节数小于文件当前大小时，`ftruncate` 掉后面的半截内容，并打一条 warn 日志。
3. 缺结尾换行的话，补一个 `\n`。
4. 下一个 `seq` 从「恢复到的最后一行的 `seq` + 1」开始——**被截掉的半行从未持久化，它的 `seq` 会被
   下一条真正写入的事件复用**，正常路径下 `seq` 从 0（meta 行）连续往上。

`replay` 走的是另一条路径（一次性从头读到尾，不做 `ftruncate`）：逐行 `getline` 解析，只有当解析失败
**且已经到达文件末尾**时才当成「写到一半的最后一行」跳过（打 warn，停止回放）；除最后一行外，任何位置
解析失败都当成真正的损坏，抛 `corrupt`。

---

## 5. list / replay

- **`list`**：只读每个会话的 meta 行、文件开头 8 KiB（给 `title_of` 用，最多喂 20 个非 meta 事件）和
  `mtime`，不会把整个文件读进来。按 `updated`（`mtime`）倒序；`project_root` 给定时，优先用
  `weakly_canonical` 后的 `git_root` 比对，`git_root` 为空时退回比对 `cwd`。读取某个会话的 meta 失败时
  跳过并打 warn，不影响其余会话。交给 `title_of` 的事件里如果有 blob 引用（比如超长的首条输入），会还原成
  原文的前 8 KiB（按字节截断，可能切在多字节字符中间），标题照样取得到，又不会为了列表去读整个大文件。
- **`replay`**：跳过 meta 行（它是信封，不是核心定义的事件；结构上也没有 `payload` 字段，回调签名
  `(type, payload)` 装不下它）。需要 meta 信息时，用 `Writer::meta()`（写入路径）或
  `Summary::meta`（`list` 路径）。`payload` 里的 `{"$blob": "...", "bytes": N}` 引用会被还原成原始字符串；
  blob 路径校验过不含 `..` 且不是绝对路径，防止越权读取会话目录外的文件。

---

## 6. UUIDv7（new_id）

前 48 位是毫秒时间戳（`unix_ts_ms`），第 7 字节高 4 位固定 `0111`（版本 7），第 9 字节高 2 位固定
`10`（RFC 4122 variant），其余是 `std::random_device` 播种 `mt19937_64` 生成的随机位。字典序和生成顺序
一致，所以会话目录名天然按时间排序，`resume`/`list` 不需要额外的索引。

---

## 7. 依赖与构建

- 只依赖 base（`base::redact` 脱敏、`base::logger` 记录 warn），编译期额外定义了
  `DAGENT_VERSION`（取自根 `CMakeLists.txt` 的 `PROJECT_VERSION`），写进每个会话的 meta 行。
- **以后才加 SQLite**：会话数量多到需要按标题或内容搜索时，引入 `sqlite3` + SQLiteCpp 做**索引**；
  JSONL 始终是事实来源，索引坏了可以随时从 JSONL 重建。
