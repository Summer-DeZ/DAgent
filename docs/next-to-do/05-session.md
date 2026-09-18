# 05 session：会话存储

库 `dagent_session`，命名空间 `dagent::session`。依赖 base。里程碑 **M3**。

## 职责

把会话里发生的事件只追加地落盘，支持列出会话、回放、恢复；超大的内容（比如工具输出）单独存放；
敏感字段在写入前脱敏。

**事件里具体放什么由核心决定**。本模块只负责外层的信封（类型、时间戳、序号）和存取，payload 对它来说是
不透明的 JSON。

## 依赖

没有第三方库，直接用 JSONL 加 nlohmann。codex 的 rollout 文件也是 JSONL：只追加写入、崩溃后能恢复、
可以直接用 `jq` 和 `grep` 排查问题。

**以后才加 SQLite**：等会话数量多到需要按标题或内容搜索时，再引入 `sqlite3`（系统库）加 SQLiteCpp，
而且只用来做**索引**。JSONL 仍然是事实来源，索引坏了随时可以重建。

## 存储布局

```
$XDG_DATA_HOME/dagent/sessions/            （默认 ~/.local/share/dagent/sessions，可配置）
└── 2026/09/18/
    └── 0199a1b2-…-7f3c/                    会话 ID 用 UUIDv7：按时间有序，文件名排序即时间顺序
        ├── events.jsonl
        └── blobs/000001.txt                超过 max_inline_payload_bytes 的内容
```

默认放在用户数据目录，而不是项目里（codex 和 opencode 都是这样），避免往仓库里写东西。`session.directory`
配置项可以覆盖这个位置。会话属于哪个项目，由 meta 行里的 `cwd` 和 `git_root` 字段判断。

```jsonl
{"type":"meta","seq":0,"ts":"2026-09-18T10:00:00.123Z","v":1,"id":"…","cwd":"/home/…","git_root":"…","model":"…","dagent_version":"0.1.0"}
{"type":"<核心定义>","seq":1,"ts":"…","payload":{…}}
{"type":"<核心定义>","seq":2,"ts":"…","payload":{"output":{"$blob":"blobs/000001.txt","bytes":183422}}}
```

## 接口草图

```cpp
namespace dagent::session {

struct Options {                              // dagent.json 的 "session" 段
    std::filesystem::path directory;          // 由 config 解析；为空时用 XDG 默认目录
    bool record_payloads = true;
    std::size_t max_inline_payload_bytes = 4096;
    std::vector<std::string> redact_fields{"api_key", "authorization", "token"};
};

struct Meta { std::string id; std::filesystem::path cwd, git_root; std::string model; /* … */ };

class Writer {
public:
    static Writer create(const Options&, Meta);                   // 新建会话
    static Writer resume(const Options&, std::string_view id);    // 在原文件末尾继续追加
    void append(std::string_view type, nlohmann::json payload);   // 脱敏 → 转 blob → 写一行
    void sync();                                                  // fsync；核心在每轮结束时调用
    const Meta& meta() const;
};

struct Summary {
    Meta meta;
    std::string title;                        // 由核心传入的提取函数生成（比如取第一条用户消息）
    std::chrono::system_clock::time_point updated;
};
std::vector<Summary> list(const Options&, const std::optional<std::filesystem::path>& project_root,
                          std::size_t limit,
                          const std::function<std::string(const nlohmann::json& first_events)>& title_of);

// 回放：blob 引用还原成原文；最后一行如果写到一半被截断了，就跳过
void replay(const Options&, std::string_view id,
            const std::function<void(std::string_view type, const nlohmann::json& payload)>&);

std::string new_id();                         // UUIDv7

class SessionError : public std::runtime_error { /* Kind: io / not_found / corrupt */ };
}
```

## 实现要点

- **每行只用一次 `write()` 写出**，文件用 `O_APPEND` 打开。**只有一个写入者**（agent 线程），不需要加文件锁。
- **崩溃恢复**：回放时，最后一行如果不是合法 JSON（写到一半进程被杀了），就丢掉这一行，打一条 warn 日志，其他行照常回放。`resume` 时先用 `ftruncate` 截掉这半行，再继续追加。
- **fsync 的时机由核心决定**（比如每轮结束调一次 `sync()`）。每写一行就 fsync 的话，流式输出会被磁盘拖慢。
- **先脱敏，再转 blob**（`base::redact`，递归处理）。
- `list` 不能把整个文件读进来：只读第一行（meta）、文件开头的几 KB（给 `title_of` 用），以及文件的 mtime。
- **UUIDv7**：前 48 位是毫秒时间戳，然后是版本位，其余是随机数（用 `std::random_device` 播种 `mt19937_64`），大约 20 行。
- 时间戳统一用 UTC ISO-8601，精确到毫秒：`std::format("{:%FT%T}Z", floor<milliseconds>(now))`。

## 验收（temp/session_check）

1. 写 1000 个事件（其中一部分超过 blob 阈值），回放得到的内容逐项相等。
2. 写入过程中 `kill -9` 进程，回放只丢掉最后半行；`resume` 以后继续追加，文件仍然合法。
3. payload 里嵌套的 `authorization` 被替换成 `***`，blob 文件里也看不到明文。
4. `list` 按更新时间倒序排列；按项目过滤时只返回当前仓库的会话；有 200 个会话时耗时在毫秒级。
5. `jq -c . events.jsonl` 检查每一行都是合法 JSON。

## 审核关注点

单次 `write` 的原子性；截断的半行在回放和恢复两条路径上是否都处理了；先脱敏再转 blob 的顺序。
