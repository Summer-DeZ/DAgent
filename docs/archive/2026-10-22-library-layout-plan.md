# agent / tui / ui 目录整理

批准：2026-10-22。Windows 移植合并后进行。第一阶段（纯移动）、第二阶段（大文件拆分）与第三阶段（剩余大文件与大头文件）均于 2026-10-22 完成并合并。

## 目标

三个最大的库（agent 6.3k 行、tui 8.9k 行、ui 7.4k 行）按职责分子目录。第一阶段只移动文件：
改 include 路径、CMake 源列表和设计文档中的文件名，不改任何代码逻辑，不拆大文件。
大文件（如 `ui/shell.cpp`、`agent/policy/permission.cpp`）的职责拆分是后续单独任务。

CMake 目标不变，仍为 `dagent_agent`、`dagent_tui`、`dagent_ui`。子目录头文件按 `agent/loop/session.hpp` 的形式引用。

## 分组

### agent

| 目录 | 内容 |
| --- | --- |
| `agent/`（根，公共词汇） | message、reply、tokens、tool_data、intent、grant、decision_types、action、events、conversation、work_plan、options、public_model、model_input、mcp_state、subagent_def、skills、identity、session_meta、log |
| `agent/port/` | 外层实现的端口，去掉 `port_` 前缀：model、tool、store、journal、decision、delegation、interaction、lease |
| `agent/policy/` | permission、authorization、decision_review、execution_registry、parent_review、execution_mailbox |
| `agent/record/` | record_codec、committer、history、recovery、compaction |
| `agent/loop/` | session、run、run_services、turn_runner、dispatch、catalog、control、model_budget |

根目录保留基础类型：外部模块引用最多的正是这些，外部 include 改动最少。

### tui（按设计文档的 L1–L7 分层）

| 目录 | 层 | 内容 |
| --- | --- | --- |
| `tui/terminal/` | L1 | terminal、event_wait；terminal.cpp、terminal_posix.cpp、terminal_win.cpp |
| `tui/grid/` | L2 | surface、grapheme；surface.cpp、present.cpp、grapheme.cpp |
| `tui/widget/` | L3–L4 | layout、widget；layout.cpp、widget.cpp、theme.cpp |
| `tui/document/` | L5 | document；document.cpp、wrap.cpp、markdown.cpp、syntax.cpp |
| `tui/runtime/` | L6–L7 | input、runtime；input.cpp、runtime.cpp、keymap.cpp |

### ui

| 目录 | 内容 |
| --- | --- |
| `ui/`（根） | shell（入口）、browser（含 posix/win） |
| `ui/style/` | strings、appearance、theme_config、display |
| `ui/state/` | projection、turn_meter、callback_gate、history_pager |
| `ui/page/` | header_bar、hint_bar、prompt_box、prompt_input、side_panel、welcome、transcript、center |
| `ui/overlay/` | approval、completion、completion_controller、panel、memory_browser、model_picker、model_dialog、settings_dialog、toast |

## 执行与验收

1. 分支 `refactor/layout`，三个库各一个提交（tui → ui → agent）：脚本 `git mv` 加 include 重写，手工改 CMake 与文档（agent.md、tui-framework.md、ui.md、platform.md 的文件名表，设计文档 README 的目录说明）。
2. Linux：dev 预设零警告；`compile_commands.json` 与 master 比对，差异只应是源文件路径；安装产物比对；`temp/platform-check/det.sh` 确定性基线与一次 TUI 冒烟。
3. Windows：虚拟机一次 Release 构建加 `--version`、`sandbox status`，与 Linux 检查并行；不跑 TUI 场景，不触发 CI。
4. 通过后合并进 master。

## 第二阶段：大文件按职责拆分

只拆同一文件里并列着几块独立职责的文件；单个长函数（`syntax.cpp` 词法器、`decision_review.cpp` 等）不动。跨文件共享的私有声明放 `*_detail.hpp`，与 `exec/detail.hpp`、`app/config_detail.hpp` 同一做法。

| 原文件 | 拆分结果 |
| --- | --- |
| `ui/shell.cpp` | `ui/shell_detail.hpp`（Shell 声明）；`shell.cpp` 生命周期、后端事件、快照应用、布局与各栏刷新；`shell_commands.cpp` 命令注册、提交、`/` 面板、权限动作与交互回答；`shell_panes.cpp` 子会话 Pane；`page/workspace.{hpp,cpp}` 排队标签、子 Agent 标签、调暗层、三栏工作区、滚动区框架；`bridge.cpp` FrontendBridge |
| `ui/page/transcript.cpp` | `transcript.cpp` 事件到 Document 的投影；`transcript_render.cpp` 各块渲染器、链接识别与 `install_renderers`；diff 宽度约定经 `diff_required_width` / `diff_available_width` 暴露 |
| `tui/document/markdown.cpp` | `markdown.cpp` 行分类与 MarkdownStream；`markdown_render.cpp` 行内解析、排版、单元格样式与 Markdown/表格渲染器；行级判定在 `markdown_detail.hpp` |
| `agent/policy/permission.cpp` | `permission.cpp` 资源分类、规则匹配、`evaluate`、模式切换；`permission_grants.cpp` 审批有效性、会话授权记忆、撤销与网络记忆；`single_use_only` 成为 Policy 的私有静态成员 |
| `agent/policy/authorization.cpp` | `authorization.cpp` ApprovalSupport 与 ActionAuthorizer；`execution_authorizer.cpp` ExecutionAuthorizer；`cancel_decision` 成为 ApprovalSupport 的静态成员 |
| `tui/grid/grapheme.cpp` | `grapheme_tables.{hpp,cpp}` 脚本生成的 Unicode 区段表（保留生成标记）；`grapheme.cpp` 只剩解码与宽度逻辑 |

第二阶段未动的 `record_codec.cpp`、`input.cpp`、`document.{hpp,cpp}`、`runtime.hpp` 在第三阶段处理（见下）；`syntax.cpp`、`decision_review.cpp` 仍不拆。

验收：每个文件一个提交，逐步零警告构建；与 master 基线的安装产物比对 `dagent::` 强符号（无删除，新增仅为外露的成员与 detail 函数）；确定性 CLI 输出一致；用基线二进制生成的 Markdown/表格/代码块会话与 edit diff 会话在新旧二进制中回放画面一致；ask 模式的 bash 审批弹窗、放行与执行正常；虚拟机 Release 构建加冒烟。

## 第三阶段：剩余大文件与大头文件

由三个子 Agent 在各自的工作树上并行完成，主会话负责分派、合并与验收。

| 原文件 | 拆分结果 |
| --- | --- |
| `agent/record/record_codec.cpp` | `record_codec.cpp` 编码；`record_decode.cpp` 解码；`decision_state` / `decision_scope` 进入公开头 |
| `tui/runtime/input.cpp` | `input.cpp` Decoder、EventRouter、InputBoxHandler；`input_decode.cpp` 转义序列解码；共享的 `SeqResult` 与解码函数在 `input_detail.hpp` |
| `tui/runtime/runtime.hpp` | `keymap.hpp`（Command、Keymap，实现仍在 keymap.cpp）；`scrollback_mouse.{hpp,cpp}`；runtime.hpp 不再包含 document.hpp |
| `tui/document/document.{hpp,cpp}` | `document.hpp` 块模型、渲染器基类与 Document；`renderers.hpp` 具体渲染器；`markdown.hpp` MarkdownStream；`scrollback.{hpp,cpp}`；共用的 `walk_line` 模板在 `document_detail.hpp` |

验收同第二阶段：全量零警告构建；强符号比对只新增 `input_detail` 与 `record_codec` 的两组函数；确定性 CLI 输出一致；两段会话回放画面一致；虚拟机 Release 构建加冒烟。
