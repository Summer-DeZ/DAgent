# 03 workspace：文件、搜索、diff、项目上下文

库 `dagent_workspace`，命名空间 `dagent::workspace`。依赖 base、exec（调用 rg 和 git）。

| 子功能 | 头文件 | 里程碑 |
| --- | --- | --- |
| 文件原语：路径解析、读写、原子写入、stale 检测 | `workspace/files.hpp` | M2 |
| 代码搜索：grep、列举文件、模糊匹配 | `workspace/search.hpp` | M2 |
| diff：unified diff 与增删统计 | `workspace/diff.hpp` | M2 |
| 项目上下文：git 信息、AGENTS.md、模板渲染 | `workspace/context.hpp` | M3 |

这几部分都是「对工作区做操作或读取工作区信息」，以后的 read/write/edit/grep/glob 工具几乎只需要依赖这一个模块。

**工具语义不在这里**：比如 edit 用什么匹配规则、read 的输出要不要带行号，这些属于 tools 层，另行讨论。

整个模块只有一个错误类型：

```cpp
class WorkspaceError : public std::runtime_error {
public:
    enum class Kind { io, stale, too_large, not_text, tool_missing, bad_pattern, bad_template };
    …
};
```

## 依赖

| 用途 | 依赖 | 引入方式 |
| --- | --- | --- |
| 文件 | 只用 `std::filesystem` + POSIX（`open`/`fsync`/`renameat`/`fstat`） | UTF-8 相关函数用 base 里的 |
| 搜索 | **ripgrep ≥ 13**（运行时依赖，本机是 14.1.1） | 通过 exec 调用，**只传 argv，不经过 shell** |
| diff | **dtl**（cubicdaiya/dtl，header-only，Myers 算法） | 放进 `src/public/lib/dtl/`，连同 LICENSE 一起 |
| 模板 | **inja**（Jinja2 语法，header-only，本身就基于 nlohmann） | FetchContent，写死 tag，加 `FIND_PACKAGE_ARGS` |
| git 信息 | **git**（运行时依赖） | 通过 exec 调用；**不用 libgit2**，太重，这里只需要读几条状态 |

选型理由：

- **rg**：codex、opencode、Claude Code 都是这么做的。.gitignore、隐藏文件、二进制文件的处理，以及多线程和速度，rg 都已经解决了。
- **dtl**：能直接生成 unified 格式的 hunk。它多年没更新，但算法是稳定的，适合 vendor 进仓库。备选方案里，`git diff --no-index` 要先写临时文件再起进程；diff-match-patch 按字符比较，不按行。
- **inja**：system prompt 里一定会出现「是 git 仓库才写这一段」「把每个 AGENTS.md 都列出来」这类条件和循环，手写字符串替换很快就不够用了。

---

## 1. 文件原语（files.hpp）· M2

### 职责

给读、写、编辑类工具提供**安全、保真**的文件操作：把路径解析到工作区以内；读文本时识别二进制、编码和
换行风格；原子写入；检测文件在「读之后、写之前」有没有被别人改过。

不做：目录遍历。列目录要遵守 .gitignore，这件事交给 search 用 `rg --files` 来做。

### 接口草图

```cpp
namespace dagent::workspace {

struct FileOptions {                           // dagent.json 的 "files" 段
    std::size_t max_read_bytes = 64 << 10;
    std::size_t max_write_bytes = 128 << 10;
};

struct Resolved {
    std::filesystem::path path;   // 绝对路径，已经规范化，符号链接已解析
    bool inside_workspace;        // 在工作区外时要不要放行，由核心做权限决策
};
Resolved resolve(const std::filesystem::path& root, std::string_view user_path);

enum class Eol { lf, crlf, mixed, none };

struct Stamp {                    // 读取时的文件指纹，写入前拿来比对
    std::filesystem::file_time_type mtime;
    std::uintmax_t size;
    bool operator==(const Stamp&) const = default;
};

struct TextFile {
    std::string content;          // 已统一为 LF、去掉 BOM、非法 UTF-8 替换成 U+FFFD
    Eol eol;
    bool bom, lossy, truncated;
    Stamp stamp;
};

enum class FileKind { text, binary, missing, directory };
FileKind probe(const std::filesystem::path&);    // 看前 8 KiB 里有没有 NUL 字节

TextFile read_text(const std::filesystem::path&, const FileOptions&);

// 原子写入：把 LF 内容还原成 eol 和 bom，写临时文件 → fsync → rename。
// 给了 expect 时，如果当前 Stamp 与它不一致，就抛 stale。
void write_text(const std::filesystem::path&, std::string_view content, Eol eol, bool bom,
                const std::optional<Stamp>& expect, const FileOptions&);
}
```

### 必须处理的坑

1. **符号链接逃逸**：`root/link → /etc`，那么 `link/passwd` 字面上在工作区里，实际上在外面。要先用 `weakly_canonical` 解析再判断前缀，**两边都要先规范化**（工作区路径本身也可能经过符号链接）。前缀比较按路径分量逐个比，不能按字符串比，否则 `/work2` 会被当成 `/work` 的子路径。
2. **换行符原样往返**：内部统一用 LF，模型看到的也是 LF，写回时还原成原来的风格。遇到 `mixed` 时按 LF 写回，并在返回值里告诉核心。
3. **原子写入**：临时文件必须建在**同一个目录**下（否则跨文件系统时 rename 会失败），用 `mkstemp` 生成，文件名形如 `.<原名>.dagent-XXXXXX`；写完先 `fsync` 文件，rename 之后再 `fsync` 目录。
4. **保留权限位**：覆盖已有文件时，要把原文件的 mode 复制给临时文件，否则 `chmod +x` 过的脚本会丢掉执行权限。目标是符号链接时，写到它指向的真实文件上，不能把链接替换成普通文件。
5. **写新文件时自动创建父目录**。
6. **用 Stamp 防止覆盖别人的改动**：用户在编辑器里保存了文件，而 agent 手里还是旧内容，这时写入必须失败。

---

## 2. 代码搜索（search.hpp）· M2

### 接口草图

```cpp
namespace dagent::workspace {

struct GrepQuery {
    std::string pattern;
    std::filesystem::path root;
    std::vector<std::string> globs;      // --glob，可以写 "!*.lock" 表示排除
    std::optional<std::string> type;     // --type cpp
    bool fixed_strings = false;
    bool case_insensitive = false;       // 不设时用 --smart-case
    bool multiline = false;
    bool hidden = false;                 // 是否包含隐藏文件；.git 目录始终排除
    int context = 0;                     // -C
    std::size_t max_matches = 200;       // 全局总数上限
};

struct Match {
    std::string path;                    // 相对 root
    std::uint64_t line;
    std::string text;                    // 已去掉行尾换行
    std::vector<std::pair<std::size_t, std::size_t>> spans;   // 匹配位置（字节偏移），给 UI 高亮用
    bool is_context = false;
};

struct GrepResult { std::vector<Match> matches; std::size_t files_with_matches; bool truncated; };
GrepResult grep(const GrepQuery&, std::stop_token = {});

struct FilesQuery {
    std::filesystem::path root;
    std::vector<std::string> globs;
    bool hidden = false;
    bool sort_by_mtime = false;          // 最近修改的排在前面
    std::size_t max_files = 1000;
};
std::vector<std::string> files(const FilesQuery&, std::stop_token = {});

// 给 @ 文件补全用：输入 "tuidoc" 能命中 "src/private/tui/document.cpp"
std::vector<std::size_t> fuzzy_rank(std::string_view needle, std::span<const std::string> haystack,
                                    std::size_t limit);
}
```

### 实现要点

- **rg 的退出码**：0 表示有匹配，**1 表示没有匹配，这是正常结果，不是错误**，2 表示出错（比如正则写错了，错误信息在 stderr 里，抛 `bad_pattern` 时带上）。
- **`--json` 输出**：每一行的 `type` 是 `begin`/`match`/`context`/`end`/`summary` 之一。路径和行内容可能是 `{"text": …}`，也可能是 `{"bytes": "<base64>"}`（非 UTF-8 时），后一种情况用 `base::base64_decode` 解码，再用 `to_valid_utf8` 转换。
- **提前停止**：`rg --max-count` 限制的是**每个文件**的匹配数，不是总数。总数达到 `max_matches` 时，通过 exec 的 stop_token 把 rg 停掉，而不是等它全部跑完再截断。
- **每行长度设上限**：加 `--max-columns 500 --max-columns-preview`，防止压缩过的 JS 这类超长行把上下文撑爆。
- `sort_by_mtime` 不要用 rg 的 `--sortr modified`，它会让 rg 变成单线程。拿到文件列表后自己 `stat` 再排序。
- rg 的路径在启动时用 `exec::which` 找一次并缓存，配置项 `search.rg_path` 可以覆盖。找不到时抛 `tool_missing`，信息里附上 `apt install ripgrep`。
- `fuzzy_rank` 用 **fzy 的打分算法**（连续匹配、单词开头、路径分隔符后的字符加分），自己移植大约 150 行。

---

## 3. diff（diff.hpp）· M2

### 职责

根据改动前后的文本生成 unified diff，并统计增删行数。用途有两个：编辑确认时的预览（TUI 已经有 diff 块
渲染器和 `diff_*` 主题令牌），以及本轮改动的汇总（`+12 −3`）。

不做：打补丁，那属于工具语义。

### 接口草图

```cpp
namespace dagent::workspace {
struct DiffStat { std::size_t added = 0, removed = 0; };
struct DiffOptions {
    int context = 3;
    std::size_t max_lines = 20000;   // 两边都超过这个行数时，不做逐行比较
};
struct Unified {
    std::string text;                // 以 "--- a/<path>\n+++ b/<path>\n" 开头；没有差异时为空
    DiffStat stat;
    bool whole_file;                 // 放弃逐行比较，按整体替换处理
};
Unified unified_diff(std::string_view before, std::string_view after, std::string_view path,
                     const DiffOptions& = {});
}
```

新建文件和删除文件时，分别用 `--- /dev/null` 和 `+++ /dev/null`，和 git 保持一致。

### 实现要点

- **按行切分时保留「最后一行没有换行符」的信息**：这种情况在 hunk 里要输出 `\ No newline at end of file`，否则生成的补丁会被 `git apply` 拒绝。
- dtl 的元素类型可以直接用 `std::string_view`，指向原文，不用复制每一行。
- **限制规模**：Myers 算法的最坏复杂度是 O((N+M)·D)。整个文件被重写时（比如被格式化了一遍），D 接近 N+M，时间和内存都会爆，所以超过上限时直接返回 `whole_file=true`。
- 输入应该是 files 已经统一成 LF 的内容，这里不处理 CRLF。

---

## 4. 项目上下文（context.hpp）· M3

### 职责

收集 system prompt 需要的**环境事实**：cwd、操作系统、shell、日期；git（是不是仓库、仓库根、分支、改动
概要、最近几次提交）；逐级向上收集的 `AGENTS.md`。另外提供模板渲染。system prompt 写什么、怎么组织，
属于核心。

### 接口草图

```cpp
namespace dagent::workspace {
struct GitInfo {
    std::filesystem::path root;
    std::string branch;                         // detached HEAD 时是短 SHA
    std::string status_summary;                 // "3 个文件已修改，1 个未跟踪"
    std::vector<std::string> recent_commits;    // git log --oneline -n 5
};
struct Instructions { std::filesystem::path file; std::string content; bool truncated; };
struct Environment {
    std::filesystem::path cwd;
    std::string os, shell, date;
    std::optional<GitInfo> git;
    std::vector<Instructions> instructions;     // 由外到内：用户全局 → 仓库根 → … → cwd
};
struct ContextOptions {
    std::chrono::milliseconds git_timeout{2000};
    std::size_t max_instructions_bytes = 32 << 10;
    std::vector<std::string> instruction_files{"AGENTS.md"};
};
Environment collect_environment(const std::filesystem::path& cwd, const ContextOptions&, std::stop_token = {});
nlohmann::json to_json(const Environment&);                              // 模板变量
std::string render(std::string_view tmpl, const nlohmann::json& data);   // inja；出错时抛 bad_template
}
```

### 实现要点

- **git 失败不能影响启动**：没装 git、不是仓库、`git status` 超时（大仓库很常见），这些情况下 `git` 字段为空，打一条 debug 日志，**不抛异常**。
- 用 `git status --porcelain=v2 --branch`：一次调用就能拿到分支和改动概要，而且输出格式稳定。
- 所有 git 调用都加 `-c core.quotepath=off`（中文路径不转义）和 `--no-optional-locks`（不和用户同时在跑的 git 操作抢锁）。
- **AGENTS.md 的收集顺序**：从 cwd 往上找到 git 根为止（不是仓库就只看 cwd），再加上 `$XDG_CONFIG_HOME/dagent/AGENTS.md`，按**由外到内**排列。越具体的指令越靠后，模型通常更重视靠后的内容。总长度超过上限时，**优先截断最外层的文件**。
- 在 inja 里注册一个 `indent(text, n)` 回调。
- 这些信息每轮对话开始时可能都要收集一次，所以几次 git 调用要并发执行。

---

## 验收（temp/workspace_check，以本仓库为对象）

**files**

1. 在工作区里建一个指向 `/etc` 的符号链接，经过它访问的路径 `inside_workspace=false`；`/work2` 不会被判成 `/work` 的子路径。
2. CRLF 文件、带 BOM 的文件，读出来再原样写回，前后逐字节一致。
3. PNG 文件被判为 binary；含非法 UTF-8 的文件 `lossy=true`，不会崩溃。
4. 读取以后另一个进程改了这个文件，再写入就抛 `stale`。
5. 有执行权限的脚本改写后仍然可执行；通过符号链接写入后，链接本身还在。
6. 写入过程中另一个进程反复读这个文件，读到的永远是完整的旧内容或完整的新内容。

**search**

7. 搜 `HttpClient` 能命中 `src/public/net/http.hpp`，并且不会出现 `build/` 下的内容（验证 .gitignore 生效）。
8. 搜不存在的字符串，结果为空，不抛异常；搜 `(` 抛 `bad_pattern`，带上 rg 的报错信息。
9. `max_matches=5` 时搜 `#include`，很快返回，并且 `truncated=true`。
10. 目录里有非 UTF-8 的文件名或内容时，bytes 形式的输出能正常解析。
11. `files` 加 `sort_by_mtime`，刚 touch 过的文件排在第一位；`fuzzy_rank("tuidoc")` 的第一名是 `src/private/tui/document.cpp`。

**diff**（用 git 来验证补丁是对的）

12. 从本仓库选几个文件，做增、删、改、首行、末行以及「末尾有没有换行」这几类改动，生成的 `.patch` 对原文件执行 `git apply --check` 全部通过；`DiffStat` 和 `git diff --no-index --stat` 一致。
13. 新建文件和删除文件的补丁也能被 `git apply`；5 万行的文件整体替换时，耗时在毫秒级，并且 `whole_file=true`。

**context**

14. 在本仓库里：branch 是 `master`，读到了根目录的 AGENTS.md，改动概要和 `git status` 一致。
15. 在 `/tmp` 下的非 git 目录里执行，或者设置 `PATH=` 让 git 找不到，`git` 字段都为空，不报错。
16. 嵌套的 AGENTS.md 能按由外到内的顺序收集到；含有 `{% if git %}` 和 `{% for … %}` 的模板渲染正确；模板有语法错误时，报错信息带行号。

## 审核关注点

- files：路径前缀判断；原子写入的三步顺序；符号链接目标的处理。
- search：退出码 1 的处理；总匹配数上限是不是通过取消 rg 实现的；base64 分支有没有处理。
- diff：「最后一行没有换行符」的处理；hunk 头的行号（从 1 开始）以及长度为 0 时的写法。
- context：git 的每一种失败是不是都没有影响启动；截断的优先级。
