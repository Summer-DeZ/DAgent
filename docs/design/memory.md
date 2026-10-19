# memory：Agent 记忆

主 Agent 用 Markdown 笔记跨会话保留用户偏好、协作反馈和项目决策。记忆由模型通过 `memory` 工具维护，
用户也可以直接编辑文件，或通过 `/memory` 查看与删除。它与人维护的 `AGENTS.md` 分开加载。

优先级为：当前用户要求 > 项目 `AGENTS.md` > 全局 `AGENTS.md` > 记忆。
用户希望遵守的规范与做法不论记为 `user`、`feedback` 还是 `project`，都是长期指令：默认应用于其覆盖的全部代码和工作，
包括独立的短代码片段，规范看起来不常见也照做；只有用户明确要求与之相反的做法或 `AGENTS.md` 另有规定时才让步，
请求中提到某个名字或只要求简短回答不算冲突。文件路径、函数、配置、过往决策等召回事实是背景，使用前先核实。
记忆不覆盖当前请求和 `AGENTS.md`。

## 1. 存储与配置

```text
home/memory/
├── global/<name>.md
└── projects/<project-key>/<name>.md
```

`HomePaths::memory` 给出记忆根，`project_memory(project_root)` 给出项目目录。项目根与工作区指令使用同一来源：
git 根，非仓库退回 cwd。project-key 为规范化项目根的目录名，加 `-` 和完整规范化绝对路径的 SHA-256 前 8 位小写十六进制字符，
例如 `DAgent-3f9a1c2e`。项目移动到另一个路径会使用另一个目录；旧格式目录不再使用，不做迁移。
目录在第一次保存时创建。
dev Home 的 `/home/memory/` 已被 git 忽略，安装也不打包本机记忆。

`config/config.json` 的独立配置段：

```json
"memory": {"enabled": true, "max_index_bytes": 8192, "max_entry_bytes": 4096}
```

省略配置段或字段时使用上述默认值；两个字节预算必须为正整数。
`enabled=false` 时不向模型注册工具，也不注入索引，已有文件与用户管理面板仍可使用。
配置在后端启动时读取，不按模型种类开关写入功能。

## 2. 条目

一条记忆一个文件，文件名与 frontmatter 中的 `name` 一致，使用小写字母、数字与单个连字符组成的 kebab-case。

```markdown
---
name: no-commit-attribution
description: 提交和 PR 不添加 Co-Authored-By 署名行
type: feedback
updated: 2026-10-18
---
## Why
用户要求提交说明只保留实际改动的信息。

## How to apply
编写提交信息或 PR 描述时，不追加 AI 署名行。
```

| type | 内容 |
| --- | --- |
| `user` | 用户身份、技术背景、沟通偏好 |
| `feedback` | 用户的纠正或认可的做法及原因 |
| `project` | 不能从代码和 git 推出的决策、约束、目标、截止时间 |
| `reference` | 外部文档、看板、工单、数据等资源位置 |

`description` 是非空单行文字，应直接表达要点。`updated` 是合法的 `YYYY-MM-DD` 日期，保存时由程序取当前本地日期，与 system 中的环境日期一致。
`feedback` 与 `project` 的正文应写明 Why 和 How to apply，截止时间用绝对日期；这些是模型写作准则，不做正文语义校验。
`max_entry_bytes` 限制含 frontmatter 的整个文件，而不只限制正文。

`tools::MemoryStore` 位于 `src/public/tools/memory.hpp` 与 `src/private/tools/memory.cpp`，tools 私有链接 yaml-cpp。
读取复用 `workspace::FileReference::read` 的普通文件校验、固定文件 FD 与 UTF-8 文本读取，frontmatter 以 yaml-cpp 解析；
扫描经固定目录 FD 遍历当前两个目录，仅读取 `.md` 普通文件。符号链接、FIFO 等非普通文件，
以及非法 frontmatter、文件名不符、非法类型或日期、超限和非法 UTF-8 条目跳过并记 warning。
不存在的目录视为空目录，不因扫描创建目录。

保存由程序生成 frontmatter，再调用 `workspace::FileReference::write` 经固定目录 FD 创建父目录，执行同目录临时文件、fsync 和 rename 的原子替换。
同 scope 同 name 覆盖更新；删除经 `FileReference::erase` 使用目录 FD 与 `unlinkat` 移除对应普通文件。
工具参数没有路径，存储层校验 scope 和 name；保存、查看、删除与列表均不跟随记忆根及其下任意路径分量的符号链接，包括悬空链接。
查看拒绝 FIFO 等非普通文件，不以阻塞方式打开它们。
查看、保存、删除遇到已有符号链接或非普通文件时，明确提示记忆条目必须是普通文件、这些类型不被允许；
后续实际操作仍由 `FileReference` 执行无符号链接解析和固定对象校验。
不维护独立索引文件，因此手工增删改条目会反映到下一次扫描。

## 3. 索引与 system

创建主会话（包括 `/new`）和恢复主会话时扫描 global 与当前项目目录。
`SessionAssembly::render_prompt` 在渲染后的 system 中追加固定英文 `# Memory` 段，与全局用户指令、Skill 段采用同一装配方式。
段落含优先级、写入准则和按 Global / Project 分组的 `name / type / updated / description` 索引，不放正文。

预算只计算索引文本（含两组标题和条目行），不包含固定写入准则。超出 `max_index_bytes` 时先淘汰全局条目，
再淘汰项目条目；同组内按 `updated` 从旧到新淘汰，同日按名字确定顺序。
保留条目在各组内按日期从新到旧显示。如果预算连两组标题也放不下，省略索引文本，仍保留固定准则。

本次会话打开期间 system 固定；本会话新写入或删除的条目不会立刻重写 system，下一次创建或恢复时重建索引。
索引在 system 中，不受历史上下文压缩影响。正文由模型按需调用 `memory(op="view", ...)` 读取。

固定英文准则要求模型：

- 保存用户画像和沟通偏好、用户纠正及认可的做法与原因、代码和 git 中推不出的项目事实、外部资源位置。
- 不保存代码结构与函数位置、git 历史、AGENTS.md 或文档已有内容、本次任务进度和临时状态、调试过程与一次性修复、密钥和凭据、个人敏感信息、未经用户确认的推测。
- description 用一句话直接写出要点，不使用「提交规范」一类空泛标题。
- 保存前检查索引两个作用域中的同类条目；修改时按索引所列的作用域和名字保存以覆盖原条目，不在旁边另存一条；删除过时或错误的条目。
- 条目按主题命名而不按当前取值命名（如 `test-command` 而非 `test-command-make-check`），以便后续变更覆盖同一条目。
- 项目作用域用于本项目，包括用户针对本项目或仓库给出的规范；全局作用域只用于跨项目的偏好和事实。
- 用户陈述的规范按原样记录，不因无法核实而省略或改写。

子 Agent 不注册 memory，也不注入该段；即便子定义显式列出 memory，也会从白名单移除。
父 Agent 应将需要的事实写入委派 prompt。

## 4. 工具、权限与记录

```text
memory(op, scope, name, type?, description?, content?)
  op: view | save | delete
  scope: global | project
```

`save` 必须提供 type、description、content；`view` 返回正文，`save` 和 `delete` 返回操作摘要。
参数错误与文件错误沿用普通工具的 `is_error` 结果，不创建另一套异常协议。

memory 是普通内置工具，使用独立的 `ToolKind::memory`。Policy 在所有权限模式以及 read_only、planning 下直接放行，
无需人工或模型审批，也不依赖 SRT。`parallel()` 不将 memory 加入只读并行组，三种操作都串行执行。
`home/memory/` 同时列入控制根受保护分类与沙箱 protected_read/protected_write；read、write、edit、bash
访问它仍遵守现有控制路径权限策略，memory 的放行不扩展这些工具的权限。

执行结果携带 `MemoryView`：`op, scope, name, type, updated, description, content`，JSON 的 kind 为 `memory`。
删除结果中仅 op、scope、name 有值。统一 View 编解码供 tool 记录、恢复、历史投影和 `tool_finished` 协议事件使用。
TUI 卡片显示操作、作用域、名字、元数据和正文；run 的 text/json 沿用工具进度摘要，jsonl 原样保留事件中的 MemoryView。

## 5. 用户管理

`/memory` 先按 Global / Project 两组列出 name、description、type、updated；Enter 查看完整元数据与正文，
详情面板支持上下滚动，选择 Delete this memory 删除，成功后重新扫描列表；Back to memories 返回列表。
这些操作不调用模型，当前 system 不随面板操作变化。

| RPC | 参数 | 结果 |
| --- | --- | --- |
| `memory.list` | `{}` | `entries: [{scope, name, type, updated, description}]` |
| `memory.view` | `{scope, name}` | `view: MemoryView`，op 为 view，含正文 |
| `memory.delete` | `{scope, name}` | `view: MemoryView`，op 为 delete |

后端经 QueryGateway 在查询线程执行，app 将存储结果映射为中立展示值，backend 不依赖 tools 实现。
这些方法作用于本后端 Home 和项目，不切换或恢复会话。字段缺失或类型错误走 invalid_params，存储失败走 query_failed。

## 6. 验证与范围

已通过构建与真实运行验收：手工条目解析与非法条目日志、save/view/delete、planning 无审批、普通工具访问记忆目录的受保护审批、
恢复后索引重建、索引预算截断、禁用配置、子 Agent 隔离，以及 `/memory` 面板的查看与删除。
效果评估见 [记忆效果评估报告](../report/memory-evaluation.md)：记忆效果高度依赖模型对写入与使用时机的判断。
本实现不包括后台抽取整合、历史会话检索、每轮检索、向量库或团队共享。
