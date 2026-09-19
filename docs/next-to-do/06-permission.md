# 06 权限

> 里程碑：C1 只做 auto / deny 两种模式的基本规则 · C2 完整规则、ask 模式、会话授权、沙箱降级 · 头文件 `agent/permission.hpp` · 实现 `permission.cpp`

tools 层只**陈述**一个调用打算做什么（`tools::Intent`），这里决定**允许、询问还是拒绝**，以及允许时给 bash 什么
沙箱。

---

## 1. 职责

**做**：按模式和默认规则判定；管理本会话的授权规则；把用户的回答变成 Grant；给界面生成「为什么要问」「会记住什么」
的说明。

**不做**：I/O。Policy 是纯逻辑，不弹对话框、不读文件；需要问的时候由调度器调 Approver（[05-dispatch](05-dispatch.md)）。

---

## 2. 接口

```cpp
namespace dagent::agent {

enum class PermissionMode {
    ask,          ///< 交互界面默认：规则说「询问」就问
    accept_edits, ///< 交互界面：工作区内的普通写入不再询问
    automatic,    ///< run 模式默认（auto 是关键字）：「询问」变成允许，但有硬性拒绝（§3）
    deny,         ///< run 模式：「询问」变成拒绝，只读 agent
};

struct Verdict {
    enum class Kind { allow, ask, deny } kind;
    tools::Grant grant;          ///< allow 时有效
    Approval approval;           ///< ask 时有效：reason、session_rule、can_network 已填好
    std::string reason;          ///< deny 时有效，进 T6
};

class Policy {
public:
    Policy(PermissionMode, exec::Support sandbox, std::filesystem::path workspace_root,
           std::filesystem::path project_root);

    Verdict evaluate(const ToolCall&, const tools::Intent&) const;
    void remember(const Approval&, const Decision&);          ///< allow_session 时记下规则
    tools::Grant grant_for(const Approval&, const Decision&) const;

    void set_mode(PermissionMode);                            ///< 线程安全（atomic）
    PermissionMode mode() const;
};

bool parallel(const Verdict&, const tools::Intent&);          ///< 05-dispatch 规则 1

} // namespace dagent::agent
```

---

## 3. 判定流程

```
evaluate(call, intent):
    1. 硬性规则（任何模式都一样）：            §4.1
    2. 默认规则表 → allow / ask / deny：      §4.2
    3. ask 时查本会话授权：命中 → allow         §6
    4. 按模式转换剩下的 ask：
         ask          → ask
         accept_edits → 工作区内普通写入 → allow；其余 ask
         automatic    → 允许（但工作区外写入、受保护文件在第 1 步已经拒绝）
         deny         → deny（reason = 「只读模式」）
```

---

## 4. 规则

### 4.1 路径分类

对 `Intent::paths` 里的每个路径（已经 resolve 过，带 `inside_workspace`）：

| 类别 | 判定 | 例子 |
| --- | --- | --- |
| **受保护** | 相对**项目根**是 `.dagent/` 下的文件、`.mcp.json`；或路径中任何一段是 `.git` | `.dagent/config.json`、`.mcp.json`、`.git/hooks/pre-commit` |
| **敏感** | 文件名匹配 `.env`、`.env.*`、`*.pem`、`*.key`、`id_rsa*`、`id_ed25519*`，或路径中有 `.ssh`、`.gnupg` 段 | `.env.dev`、`~/.ssh/config` |
| 工作区外 | `inside_workspace == false` | `/etc/hosts`、`../other/a.cpp` |
| 普通 | 其余 | `src/a.cpp` |

受保护的两类文件能改网关地址、启动任意命令；项目已受信任时，模型改了它们，下次启动就生效。`.git/` 里有 hooks。
所以对它们的**写入**永远要用户亲自确认。

### 4.2 默认规则表

| Intent | 条件 | 默认 | Grant / reason |
| --- | --- | --- | --- |
| read（read/grep/glob） | 所有路径普通且在工作区内 | 允许 | — |
| read | 有工作区外路径 | 询问 | 「读取工作区外的文件」 |
| read | 有敏感路径 | 询问 | 「读取可能含密钥的文件」 |
| write（edit/write） | 普通，工作区内 | 询问 | 「修改文件」；Approval 带 diff 预览 |
| write | 工作区外 | 询问，**不提供会话授权**；auto 模式**拒绝** | 「修改工作区外的文件」 |
| write | 受保护 | 询问，**不提供会话授权**，accept_edits 不豁免；auto 模式**拒绝** | 「修改 agent 配置 / git 内部文件」 |
| exec（bash） | `known_readonly`，沙箱可用 | 允许 | `read_only` 沙箱 |
| exec | 其他，沙箱可用 | 询问 | 「运行命令」；允许后 `workspace_write`，默认不联网 |
| exec | 沙箱不可用 | 询问，含只读命令；auto 模式**允许但记 warn** | 「当前系统不支持沙箱，命令会不受限制地运行」；`full_access` |
| external（MCP） | — | 询问 | 「调用外部工具 {server}.{tool}」 |

- **沙箱可用** = `exec::probe()` 的 `landlock_abi > 0 && seccomp`，启动时探测一次放进 `Setup`。exec 文档要求：不支持的
  机器上必须降级为询问。
- `known_readonly` 只是静态分析的结论，所以自动放行时仍然给 `read_only` 沙箱，不给 `workspace_write`。
- 沙箱可写路径统一是 `{工作区根, /tmp}`（tools 的 bash 已经这样做）。

### 4.3 auto 模式的硬性拒绝

run 模式下没人能确认，所以 `automatic` 把「询问」变成允许，但以下情况直接拒绝（T6，reason 写清楚是哪条）：

- 写工作区外的文件；
- 写受保护文件；
- bash 要求联网——auto 模式的 bash 一律 `allow_network = false`，这不是拒绝调用，而是不给网络（命令里的网络访问会失败，
  tools 层会给模型沙箱提示）。

---

## 5. 询问

`Verdict::ask` 带着一个填好的 `Approval`：

| 字段 | 内容 |
| --- | --- |
| `reason` | 上表的 reason |
| `intent` | 拷贝一份（界面要显示 summary、diff 预览、完整命令） |
| `session_rule` | 选「本会话允许」会记住什么，用人话写（§6）；不提供会话授权时为空 |
| `can_network` | bash 且沙箱可用时为 true |

调度器调 Approver 拿到 `Decision`：

| Answer | Grant | 之后 |
| --- | --- | --- |
| `allow` | bash：`workspace_write`，`allow_network = decision.network` | 执行 |
| `allow_session` | 同上 | `remember` 后执行 |
| `deny` | — | T3，本轮以 denied 结束 |
| `deny_with_feedback` | — | T4，本轮继续 |

拒绝后停下来等用户，而不是让模型继续：被拒绝后换个方式硬来，是最让人烦的 agent 行为。用户想让它换做法时，用
「拒绝并说明」。

---

## 6. 会话授权

| Intent | 记住的规则 | `session_rule` 示例 |
| --- | --- | --- |
| write（普通） | 切到 accept_edits 语义：本会话内工作区普通文件的写入不再询问 | 「本会话内修改工作区文件不再询问」 |
| exec | 命令前缀集合（§6.1） | 「以后 `npm test`、`git commit` 不再询问」 |
| external | 这个工具的 `qualified_name` | 「本会话内调用 github.create_issue 不再询问」 |
| read（工作区外） | 路径所在目录 | 「本会话内读取 /usr/include 下的文件不再询问」 |
| read（敏感） | 不提供 | — |

会话授权只在内存里，不写配置，恢复会话时不继承——它是「我现在盯着」的授权，不是长期信任。

### 6.1 bash 前缀规则

```
prefix(simple_command):
    argv[0]，再加上第一个不以 - 开头的参数（如果有）
    例：git commit -m x → "git commit"；npm run build → "npm run"；make -j8 → "make"；./build.sh → "./build.sh"

记住：exec::analyze(command) 里每一条不是已知只读的简单命令的 prefix
命中：has_opaque == false，并且每一条简单命令要么单独看是已知只读，要么它的 prefix 在集合里
```

| 已记住 | 新命令 | 结果 |
| --- | --- | --- |
| `npm test` | `npm test -- --watch=false` | 放行 |
| `npm test` | `cd web && npm test` | 放行（匹配时忽略 `cd`：它只改变后续命令的目录） |
| `npm test` | `npm test && rm -rf dist` | 询问（`rm` 没记住） |
| `make` | `make $(nproc)` | 询问（`has_opaque`） |
| `git commit` | `git push` | 询问（前缀不同） |

「单独看是否只读」：用只含这一条简单命令、`has_opaque = false` 的 `exec::Analysis` 调 `is_known_readonly`。`cd`
不在 exec 的只读白名单里，但匹配时同样忽略：提示词让模型用 `cd dir && …` 换目录，不忽略它，`make` 这样的规则就永远匹配
不上 `cd build && make`；后续命令仍要各自命中规则。

前缀取「命令名 + 第一个非选项参数」，所以 `curl https://a` 记住的是 `curl https://a`，换一个 URL 仍会询问。

放行的命令用 `workspace_write` 沙箱、不联网；当初授权时如果勾了联网，这条规则也记住联网。

---

## 7. 权限模式从哪来

| 场景 | 模式 |
| --- | --- |
| 交互界面 | 启动时 `ask`；Shift+Tab 在 `ask` ↔ `accept_edits` 之间切换，状态栏显示 |
| `dagent run` | `--permissions` 给了就用它；否则用配置的 `permissions`；都没有是 `auto` |

`Args::permissions` 现在默认值是 `"auto"`，分不清用户是否显式传了参数，要改成 `std::optional<std::string>`
（[11-entry §6](11-entry.md)）。配置里的 `permissions` 从此只作用于 run 模式。

---

## 8. 记录

每次**询问过**的决定写一条 `permission` 记录（[09-record §3](09-record.md)）：`{call_id, answer, rule, network}`。
直接放行和策略拒绝不写（它们可以从规则推出来），只记用户亲手做的决定，便于事后审计「谁批准了这个 `rm`」。

---

## 9. 已知限制

- **bash 能绕过受保护文件规则**：Landlock 是白名单，没法在可写的工作区里再排除 `.mcp.json`、`.dagent/`。
  `workspace_write` 的 bash 可以改它们。缓解办法（以后做）：启动时发现这两个文件在上次会话之后被修改过，重新询问
  一次信任。
- **读敏感文件的保护只针对 read/grep/glob**：只读 bash（`cat .env`）会被自动放行。要堵这个口子，需要把敏感路径
  规则也用到 bash 的参数上，但参数里的路径不一定能静态识别，第一版不做。

---

## 10. 验收

- C1：plan 场景 1 在 `auto` 模式下跑通。
- C2：plan 场景 6（deny 模式、auto 的硬性拒绝）、场景 7（auto 下 bash 不联网）。
- C4：plan 场景 14（对话框四种回答、会话授权的前缀规则）。
- 沙箱降级：在 `temp/core_check` 里把 `Setup::sandbox` 手动设成不支持，确认只读 bash 也会询问（交互）/ 以
  `full_access` 运行并记 warn（auto）。
