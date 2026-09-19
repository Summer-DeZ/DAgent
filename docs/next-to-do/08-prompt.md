# 08 提示词

> 里程碑：C1（`compact.md` 在 C5）· 头文件 `agent/prompt.hpp` · 实现 `prompt.cpp` + 构建目录里生成的 `prompts.cpp` · 依赖 workspace（context、render）

system prompt 决定了模型会不会按我们设计的方式使用工具，C1 能不能通过验收很大程度取决于它。**第一版要在写 C1 代码
之前写好**，不要留到最后。

---

## 1. 职责

**做**：内置提示词文本（编进二进制）；开发期覆盖；会话开始时渲染一次 system prompt。

**不做**：每轮动态改 system prompt（破坏前缀缓存，[03-conversation §4](03-conversation.md)）；工具说明（在 tools 层
各 `.cpp` 里，和 Schema 放在一起）。

---

## 2. 文件

| 文件 | 用途 | 模板变量 |
| --- | --- | --- |
| `prompts/system.md` | 主 system prompt | 有（§3） |
| `prompts/compact.md` | 摘要请求的 system prompt（[07-context §4.4](07-context.md)） | 无 |

两个都是 inja 模板语法（`{{ }}`、`{% %}`），用 `workspace::render` 渲染；`compact.md` 不用变量，但也走一遍 render，
保持处理方式一致。

### 2.1 编进二进制

```cmake
# src/CMakeLists.txt
file(READ ${CMAKE_SOURCE_DIR}/prompts/system.md  DAGENT_PROMPT_SYSTEM)
file(READ ${CMAKE_SOURCE_DIR}/prompts/compact.md DAGENT_PROMPT_COMPACT)
configure_file(${CMAKE_SOURCE_DIR}/cmake/prompts.cpp.in ${CMAKE_BINARY_DIR}/generated/prompts.cpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_SOURCE_DIR}/prompts/system.md ${CMAKE_SOURCE_DIR}/prompts/compact.md)
target_sources(dagent_agent PRIVATE ${CMAKE_BINARY_DIR}/generated/prompts.cpp)
```

```cpp
// cmake/prompts.cpp.in
#include "agent/prompt.hpp"
namespace dagent::agent {
std::string_view builtin_system_prompt()  { return R"DAGENT_PROMPT(@DAGENT_PROMPT_SYSTEM@)DAGENT_PROMPT"; }
std::string_view builtin_compact_prompt() { return R"DAGENT_PROMPT(@DAGENT_PROMPT_COMPACT@)DAGENT_PROMPT"; }
}
```

- 改了 `prompts/*.md`，`CMAKE_CONFIGURE_DEPENDS` 让下一次构建自动重新 configure，不需要手动重跑 cmake。
- 提示词里不能出现 `)DAGENT_PROMPT"`（不会有人写这个）。GCC 13 没有 `#embed`，所以用 configure_file。
- 这是仓库里唯一一个在构建目录生成的 `.cpp`。AGENTS.md 规定 cpp 只能放 `src/private`，指的是手写源码；如果你不接受
  生成文件，替代方案是把提示词写成 `src/private/agent/prompt_text.cpp` 里的原始字符串，代价是改提示词要改 C++ 文件。

### 2.2 开发期覆盖

`gateway.system_prompt_file` 不为空时，读这个文件代替内置的 `system.md`（app 已经把它解析成绝对路径）。读不到 →
启动失败，退出码 1，提示文件路径。开发配置 `config/dagent.json` 指向 `../prompts/system.md`：开发时改完提示词不用
重新构建。

---

## 3. 渲染

```cpp
struct PromptVars {
    std::string model;
    std::filesystem::path project_root;
    bool sandbox;                  ///< exec::probe() 结果：沙箱可用
    std::string permission_mode;   ///< "ask" / "accept_edits" / "auto" / "deny"
};

std::string render_system_prompt(std::string_view tmpl, const workspace::Environment&, const PromptVars&);
```

**只在会话开始时渲染一次**（`Agent::create`；恢复时按当前环境重新渲染一次）。之后日期变了、git 状态变了都不更新：
system prompt 一变，前缀缓存整个失效。

模板可用的变量：

| 变量 | 来源 | 例子 |
| --- | --- | --- |
| `cwd` | `Environment::cwd`（= 工作区根） | `/home/jyt/DAgent` |
| `os`、`shell`、`date` | `Environment` | `Linux 6.17`、`zsh`、`2026-09-19` |
| `git` | `Environment::git`，不在仓库里时为 `null` | `{root, branch, status_summary, recent_commits}` |
| `instructions` | `Environment::instructions`，由外到内 | `[{file, content, truncated}]` |
| `model` | `PromptVars` | `deepseek-chat` |
| `project_root` | `PromptVars` | 同上 |
| `sandbox` | `PromptVars` | `true` |
| `permission_mode` | `PromptVars` | `ask` |

`system.md` 顶部用 inja 注释 `{# … #}` 列出这张表，改模板的人不用来翻文档。

---

## 4. 内容要点

### 4.1 system.md

按这个顺序组织，每节尽量短（整体控制在 2k token 以内，不含 AGENTS.md）：

1. **身份**：你是 DAgent，一个在用户终端里工作的编码 agent；用用户使用的语言回复。
2. **环境**：工作目录、系统、shell、日期、git 分支和状态（`{% if git %}`）。
3. **工作方式**
   - 先了解再动手：用 grep / glob / read 弄清楚相关代码，再改。
   - 只做用户要求的事；发现别的问题可以提，不要顺手改。
   - 改完要验证：能构建就构建，有相关测试就跑。
   - 做完用一两句话说明改了什么、验证结果如何；没验证就明说。
4. **工具规则**（补充工具说明里说不全的、跨工具的约定）
   - 改文件前必须先 read；edit 的 `old_string` 不要带 read 输出的行号前缀。
   - 找文件和内容优先用 glob / grep，不要用 bash 的 `find`、`grep`。
   - **互不依赖的读取放在同一次回复里一起调用**——调度器会并行执行它们（[05-dispatch](05-dispatch.md)）。
   - bash 每次都是新进程：需要换目录就写 `cd dir && …`；不支持后台常驻进程。
   - `{% if sandbox %}` bash 在沙箱里运行：只能写工作目录和 /tmp，默认不能联网；遇到沙箱报错不要反复重试，向用户说明。`{% endif %}`
5. **上下文**：旧工具输出可能被裁剪或摘要；读到后续需要的信息先在回复中记下路径、namespace、接口和结论，再调用下一个工具。
   看到「旧的工具输出已省略」且摘要或回复没有所需事实时重新调用，不凭记忆描述，不根据文件名猜测职责。
6. **权限**：部分操作需要用户确认；**用户拒绝后停下来等指示，不要换一种方式绕过**。`{% if permission_mode == "auto" or permission_mode == "deny" %}` 当前是非交互模式，没人能确认，被策略拒绝的操作要在最终回复里说明。`{% endif %}`
7. **输出格式**：回复用 Markdown，界面会渲染；引用代码写 `path:line`；不要把大段文件内容贴进回复。
8. **项目指令**：逐个插入 `instructions`，每个前面标出来源文件：

   ```
   {% for i in instructions %}
   ## 项目指令：{{ i.file }}
   {{ i.content }}
   {% endfor %}
   ```

   项目指令放在最后：它最长、最常变，放后面对前面部分的缓存最友好。

### 4.2 compact.md

- 你在为一段编码对话写交接摘要，读者是接手的同一个 agent，它看不到原始对话。
- 必须包含的六项（[07-context §4.4](07-context.md)），**用户请求原文照抄**，文件列表每行「路径：改了什么」。
- 只写事实，不写客套，不复述工具输出原文，不超过 1500 字。
- 输出格式固定成六个小标题，便于模型接手时查找。
- 本地 Qwen 实测后补充：逐字核对路径、文件名和限定条件；再次摘要时原样转抄旧摘要里的请求，原文完整性优先于字数限制。
  明确工具输出占位只表示裁剪，不表示调用失败或工作未完成；从实际调用和 assistant 已有结论保留进度，避免反复读取。
- 摘要请求保留能容纳的工具输出原文（07-context §4.3）；提示词要求从原文提取用户所需的事实，即使 assistant 尚未写下结论，
  也在「当前进度」保留 namespace、接口、路径和报错等信息，避免只记「读过文件」而丢失结果。

---

## 5. 失败

| 情况 | 处理 |
| --- | --- |
| 模板语法错误 | `workspace::render` 抛 `bad_template`（带行号）→ `Agent::create` 抛出 → 启动失败，退出码 1 |
| 覆盖文件读不到 | 启动失败，退出码 1 |
| AGENTS.md 过大 | workspace 已按 `max_instructions_bytes`（32 KiB）截断并标 `truncated`；模板里 `{% if i.truncated %}` 提示模型「已截断」 |
| git 超时或不可用 | `git` 为 `null`，模板照常渲染 |

内置模板的语法错误在开发时就会暴露（每次启动都渲染），不会带到用户那里。

---

## 6. 可观测性

渲染结果写进会话记录的 `system` 事件（[09-record §3](09-record.md)，大内容由 session 自动转 blob）。排查「模型为什么
这样做」时，`jq` 就能看到它当时拿到的完整提示词。恢复会话时不读它（按当前环境重新渲染），它只用于排查。

---

## 7. 验收

- C1：plan 场景 1 通过本身就是对 system.md 的检验。另外人工检查一次渲染结果：在仓库根和非 git 目录各启动一次，
  确认 git 段、AGENTS.md 段正确出现或消失。
- C2：plan 场景 5 里，模型在提示词引导下确实把多个 read 放进了同一次回复。
- C5：plan 场景 17 的摘要满足 §4.2 的格式。
