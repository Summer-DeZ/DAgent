# workspace：文件、搜索、diff、项目上下文

给读、写、编辑、搜索类工具提供的底座。头文件在 `src/public/workspace/`，实现在 `src/private/workspace/`，
构建为静态库 `dagent_workspace`，命名空间 `dagent::workspace`。依赖 base 和 exec（调用 `rg`、`git`）。

**工具语义不在这里**：edit 用什么匹配规则、read 的输出要不要带行号，属于 tools 层。这里只做「对工作区
做操作或读取工作区信息」。

整个模块只有一个错误类型：

```cpp
class WorkspaceError : public std::runtime_error {
public:
    enum class Kind { io, stale, too_large, not_text, tool_missing, bad_pattern, bad_template, cancelled };
    Kind kind() const noexcept;
};
```

`cancelled` 对应 `net::HttpError` 已有的先例：`std::stop_token` 被外部取消时抛出，和「无匹配」「已截断」
这些正常结果区分开。

---

## 1. 概述

| 能力 | 头文件 | 主要接口 |
| --- | --- | --- |
| 文件原语：路径解析、固定目录/对象、文本读写、原子写入、stale 检测 | `workspace/files.hpp` | `resolve`、`FileReference`、`probe`、`read_text`、`write_text`、`stamp_of` |
| 代码搜索：grep、列举文件、模糊匹配 | `workspace/search.hpp` | `grep`、`files`、`fuzzy_rank` |
| diff：unified diff 与增删统计 | `workspace/diff.hpp` | `unified_diff` |
| 项目上下文：git 信息、AGENTS.md、模板渲染 | `workspace/context.hpp` | `collect_environment`、`to_json`、`render` |

所有接口都遵循外围模块的统一形状：在调用线程上阻塞执行，取消走 `std::stop_token`，配置走各自的
`XxxOptions` 结构体（不读配置文件）。

---

## 2. 文件原语（files.hpp）

`files.cpp` 保留路径解析、路径包含判断、符号链接目标计算和 BOM/EOL/UTF-8 编解码。
`files_detail.hpp` 声明模块内部的 `fail`、`through_symlink`、`decode_text`、`encode_text` 与
`encode_reference_text`，不传递原生句柄或 stat 类型。两条编码函数分别保留原 reserve/逐字符追加方式。
`files_posix.cpp` 实现文件 I/O、权限、原子替换和 `FileReference` 的全部原生状态；
CMake 通过 `files_${DAGENT_PLATFORM}.cpp` 选择它，`files.hpp` 的对外接口不变。

### 路径解析：resolve

把用户路径解析成绝对路径：相对路径基于 `root`；两侧都用 `weakly_canonical` 规范化并解析符号链接
（不存在时退化成 `absolute` + `lexically_normal`，不报错）。判断是否在工作区内时按**路径分量逐个比较**
（`fs::path` 的迭代器天然按分量拆分），不是字符串前缀比较，所以 `/work2` 不会被判成 `/work` 的子路径。

在工作区外时放不放行，由核心根据 `Resolved::inside_workspace` 决定。
非空用户路径经 `base::path_from_utf8` 进入文件系统；POSIX 保留原始字节，不提前校验 UTF-8。
文件错误中的路径经 `base::path_to_utf8` 输出，原错误类型与文本格式不变。

### 工具执行：FileReference

模型的 read/edit/write 使用 `FileReference`，不直接以路径调用通用读写函数。构造只获取元数据，
以 `O_PATH` 固定已存在的祖先目录和目标对象；`openat2` 禁止穿过新符号链接或 magic link。
工具层先消费 Policy 的读/写 grant，再调用以下操作；本模块自身不决定权限。

- `validate()` 重开路径核对 device/inode；父目录、目标被替换或原本不存在的目标已出现时返回 stale，要求重新准备。
- `read()` 只读已固定的普通文件，保留文本读取、上限、换行与 UTF-8 语义；目录读取通过 `directory_path()` 取得固定目录。
- `write()` 在获准目录句柄下逐级创建缺失父目录，写同目录临时文件，核对对象和 `expect` 后用 `renameat2` 提交并同步目录。
  新建文件使用 `RENAME_NOREPLACE`，避免覆盖准备后出现的同名文件。
- `Stamp` 包含 mtime、size、device、inode；预览和写入都核对，FileTracker 不能代替访问授权。

`read()` 先 validate、确认普通文件，再以 `O_RDONLY | O_CLOEXEC` 打开固定 inode 的 `/proc/self/fd` 路径，
执行 fstat、限长读取和共享解码，返回时关闭读取句柄；原生锚点与目标句柄的持有期不变。

`write()` 先 validate、检查大小与普通文件类型，再复制锚点句柄，逐级 mkdirat/openat2 创建并固定父目录。
首次 expect 校验后调用 `encode_reference_text`，在 `/proc/self/fd/<目录>/.dagent-XXXXXX` 创建临时文件，
旧文件权限保留 `0777`，新文件使用缓存的默认权限。写入遇 EINTR 重试，零字节或其他失败均报 io。
文件 fsync 后再次 validate 和 expect 校验，再 renameat2；同步目录、fstat 临时文件句柄取得返回 Stamp。
临时文件句柄一直保持打开，退出作用域时先关闭同步目录句柄，再 unlinkat 临时名称、关闭临时文件与父目录句柄；
失败路径使用相同清理对象，不提前关闭或增加重试。

下文 `probe/read_text/write_text` 是装配等受信任调用使用的通用原语，不具备上述审批对象固定契约。

### 通用读取：probe / read_text

- `probe`：看前 8 KiB 里有没有 `\0` 字节；文件不存在（`ENOENT`/`ENOTDIR`）返回 `missing`，不抛异常。用
  `O_NONBLOCK` 打开，避免在 FIFO 上卡住；`EAGAIN` 时按 `text` 处理（FIFO 暂时没数据，不代表是二进制）。
- `read_text`：整篇内容里出现 `\0` 就抛 `not_text`；超过 `max_read_bytes` 时截断内容并置 `truncated=true`
  （截断后额外探测一个字节，确认原文件确实更长，避免正好读满上限时误判）。
- **换行风格**：`detect_eol` 统计 `\r\n`/`\n`/单独的 `\r`，三类都出现过的判 `mixed`。内部统一转成 LF
  （`to_lf`），BOM（`EF BB BF`）单独摘出来，`bom=true`。
- **编码**：转 LF 后校验 UTF-8；非法字节用 `base::to_valid_utf8` 替换成 U+FFFD，`lossy=true`。
- `Stamp`（`mtime`、`size`、`device`、`inode`）来自 `fstat`，`mtime` 精度到纳秒。

### 通用写入：write_text

1. 检查 `max_write_bytes`。目标是符号链接时，`through_symlink` 落到它指向的真实文件（相对链接相对链接所在目录解析）；找不到
   目标时保持原样，让后续步骤给出真实的 I/O 错误。
2. `create_directories` 创建父目录，再调用 `encode_text` 按 `eol`/`bom` 编码（`crlf` 时把每个 `\n` 前插入 `\r`）。
3. `stat` 当前文件，拒绝目录；有 `expect` 时比较 Stamp，不一致（文件被改过或消失）就抛 `stale`。
4. 临时文件建在**同一目录**下（`.{文件名}.dagent-XXXXXX`，`mkstemp` 生成），用 fchmod 保留旧文件 `07777` 权限位。
   新文件按进程 umask 取 `0666`；`default_file_mode` 从 `/proc/self/status` 读取并静态缓存，失败回退 `0644`，
   不调用会改变全局状态的 umask。两条写入路径共用这份缓存。
5. 写入遇 EINTR 重试，其他负返回报 io；零字节返回仍按原循环处理，不新增报错。
   `fsync` 文件后显式 `close`，即使 close 报错也先将持有值置为 -1；随后 `rename` 到目标并标记保留。
6. 打开父目录并 `fsync`（打开或同步失败均忽略）。失败清理保持先关闭仍持有的临时句柄、再 unlink 临时路径。

`stamp_of` 返回当前文件指纹，文件不存在时返回 `nullopt`（不抛异常），用于写入前的比对或核心自己判断
文件是否被外部改动。

---

## 3. 代码搜索（search.hpp）

### grep / files：调用 rg

- rg 路径：`SearchOptions::rg_path` 非空时使用它（不含 `/` 时当命令名用 `exec::which` 在 PATH 里找，并检查可执行位），否则用 `exec::which("rg")` 查 PATH
  并用静态局部变量缓存。都找不到时抛 `tool_missing`，信息里带 `apt install ripgrep`。
- `grep` 固定加 `--no-config --json --no-messages --color=never --max-columns 500 --max-columns-preview`；
  `case_insensitive` 不设时用 `--smart-case`；模式串前面加 `--`，不会被解析成选项。
- `GrepQuery::root` 是普通文件时只搜这一个文件：rg 的 cwd 换成它所在的目录，文件名作为路径参数传入
  （显式给出的路径 rg 不做忽略规则过滤），`Match::path` 相对这个目录。
- **退出码**：0/1（有/无匹配）都当正常结果处理；2 且 stderr 非空时抛 `bad_pattern`（带上 rg 的报错），
  否则抛 `io`。
- **`--json` 事件流**：按行解析，`type` 为 `match`/`context` 时取 `data.path`、`data.lines`
  （`text` 或 `bytes` 二选一，后者用 `base::base64_decode` + `to_valid_utf8` 解码），`context` 行不计入
  `max_matches`、不产生 `spans`。
- **提前停止**：命中数达到 `max_matches` 时，通过内部 `stop_source`（外部 `stop_token` 通过
  `stop_callback` 转发到同一个 `stop_source`）取消只读 SRT 执行，不等 rg 自然结束；`ExecError::cancelled`
  在「自己主动停」时按正常结果处理，只有外部 `stop_token` 触发的取消才继续抛成
  `WorkspaceError::cancelled`。
- `files` 用 `--no-config --files --null`；不按 mtime 排序时可以在凑够 `max_files` 后立即停 rg；要按 mtime 排序时
  必须拿到全量结果再自己 `stat` 排序——`rg --sort` 会让 rg 退化成单线程，所以不用它。

搜索通过 `SearchOptions::sandbox` 的 `ReadOnlySandbox::run` 启动，缺少 SRT 时失败，不提供宿主回退。
app 默认注入托管 rg 的绝对路径；工具层按 grant 设置可读范围并过滤结果，补全查询也过滤控制面与敏感路径。

### fuzzy_rank：移植的 fzy

逐行移植 fzy 的打分算法（`bonus_for` 的加分表：路径分隔符后 0.9、`-`/`_`/空格后 0.8、`.` 后 0.6、
camelCase 边界 0.7；连续匹配奖励 1.0；两端与内部的 gap 惩罚分别是 -0.005/-0.005/-0.01）。
先用 `is_subsequence` 过滤掉不可能命中的候选，再对剩下的调
`fzy_score`，`std::stable_sort` 按分数从高到低排。

分数很接近的候选之间的相对顺序完全由算法决定，不做任何路径偏好之类的人工调整——这是忠实移植换来的
可预测性，代价是遇到几个分数只差千分位的候选时，谁第一名可能和直觉不完全一致。

---

## 4. diff（diff.hpp）

- 按行切分时**保留行尾的 `\n`**：最后一行没有换行符时，它和「有换行」的行天然不 `==`，据此在输出里补
  `\ No newline at end of file`，不用另外记一个标志位。
- 用 dtl 的 `Diff<std::string_view>` 做 Myers 算法比较，元素是指向原文的 `string_view`，不复制每一行。
- **hunk 合并**：把连续的非 `SES_COMMON` 段前后各留 `context` 行；如果下一段的起点落在上一个 hunk 的
  终点以内，直接合并成一个 hunk。hunk 头 `@@ -old_start,old_count +new_start,new_count @@` 里，
  某一侧行数为 0 时对应的起始行号也写 0（和 `git diff` 一致）。
- **规模限制**：两侧行数都超过 `max_lines`（默认 20000）时放弃逐行比较，整个文件当作一次替换
  （`whole_file=true`），避免 Myers 算法在大改动上的 O((N+M)·D) 退化。
- 新建/删除文件分别用 `--- /dev/null` 和 `+++ /dev/null`，和 `git diff` 一致。
- 输入必须是已经统一成 LF 的文本（files 读出来的就是），这里不处理 CRLF。

---

## 5. 项目上下文（context.hpp）

### 环境事实

`collect_environment` 收集 cwd（规范化）、`detail::os_description` 提供的 os 字符串、`$SHELL`（没有时退回 `sh`）、本地
日期；调用前 `stop_token` 已经被取消时直接抛 `cancelled`，不做任何工作。
OS 描述由 `context_posix.cpp` 用 uname 拼出 `sysname release`，失败仍返回 `unknown`；
日期转换调用 `base::local_time`，格式化和失败时留空的处理仍在共享实现。

### git 信息

`rev-parse --show-toplevel`、`status --porcelain=v2 --branch`、`log --oneline -n 5` 三条命令并发跑
（`std::async`），都带 `--no-optional-locks`（不和用户正在跑的 git 抢锁）和 `-c core.quotepath=off`
（中文路径不转义）。**任何一步失败都不抛异常**：没装 git、不是仓库、超时、被取消，`run_git` 统一捕获
异常并返回「未成功」，只在 debug 日志里记一行；`root` 或 `status` 没成功时整个 `GitInfo` 为空
（`log` 单独失败不影响，只是拿不到最近提交列表）。

当前上下文采集调用 `/usr/bin/git`，通过 `ContextOptions::sandbox` 进入只读 SRT；禁用 fsmonitor、hooks、
pager 和全局/系统 Git 配置。这里尚未切换到托管 Git，宿主无该路径或 SRT 不可用时 Git 信息为空。

`status --porcelain=v2` 的输出里，`1 `/`2 ` 开头的行计入「已修改」，`u ` 计入「冲突」，`? ` 计入
「未跟踪」；`branch.head` 是 `(detached)` 时用 `branch.oid` 的前 7 位当分支名。
分支名和近期提交文本修复为合法 UTF-8，供 system 模板和 workspace 查询展示。仓库根保留原始路径字节参与
规范化和指令文件查找；路径仅在转换为模板展示数据时修复，不以展示副本做文件操作。
Git 根目录通过 `base::path_from_utf8` 转为路径；上下文序列化先用 `path_to_utf8` 取路径文本，再沿用
`to_valid_utf8` 清洗。HEAD diff 的 `-C` 和 `ls-files` 路径使用原生文本接口，`HEAD:` 后的路径及 diff 文件名
使用 `path_to_generic_utf8`；Git 选择、PATH 继承和异常捕获位置保持原样。

### AGENTS.md 收集

候选路径是从 cwd 向上到 git 仓库根（不是仓库时只有 cwd 自己）逐级的 `AGENTS.md`。最终顺序**由外到内**：
仓库根最先，越往 cwd 靠近的越后面。不读取 HOME 或 XDG 下的全局指令文件。

预算不足时优先牺牲最外层：从最内层往外分配 `max_instructions_bytes`，分配不到完整内容的文件先截断
（`base::truncate_middle`），预算耗尽后，更外层的文件整体丢弃（`content` 为空，`truncated=true`）。

### 模板渲染

`to_json` 把 `Environment` 转成 inja 能用的 JSON（`git` 为空时输出 JSON `null`）。`render` 用 inja 渲染，
额外注册了一个二元回调 `indent(text, width)`（按行加前导空格，用于把多行内容嵌进缩进的模板位置）。
inja 抛 `InjaError` 时，转成 `bad_template`，如果错误带了行号就拼进消息（`line {n}: ...`）。

---

## 6. 依赖与构建

- **dtl**（cubicdaiya/dtl，header-only）：vendor 在 `src/public/lib/dtl/`，附带原始 LICENSE。选它是因为
  能直接生成 unified 格式的 hunk；多年没更新，但 Myers 算法本身是稳定的，适合整份放进仓库。
- **inja**（pantor/inja v3.5.0，header-only，Jinja2 语法）：通过 `cmake/deps.cmake` 的 `FetchContent`
  引入，`FIND_PACKAGE_ARGS 3.5`（系统有就用系统的）。它本身依赖 nlohmann/json，`deps.cmake` 里把已经
  vendor 在 `src/public/lib/nlohmann/` 的那份包成 `nlohmann_json::nlohmann_json` interface target 给
  inja 用，避免同一个翻译单元里出现两份 nlohmann（会撞 include guard 和 ODR）。
- **ripgrep ≥ 13**、**git**：运行时依赖，调用方提供 argv，`ReadOnlySandbox` 逐参数引用后交给 SRT 执行，不拼接未经引用的用户参数；不是编译期依赖。
  找不到 `rg` 时 `search.rg_path`（`home/config/config.json`）可以覆盖自动探测的路径。
- `dagent_workspace` 对外公开链接 `dagent_base`，`dagent_exec` 和 `inja` 在 target 中声明为私有链接依赖；
  `search.hpp`、`context.hpp` 的公开类型仍引用 exec 的只读 SRT 配置。
