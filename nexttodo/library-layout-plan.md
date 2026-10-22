# agent / tui / ui 目录整理

批准：2026-10-22。Windows 移植合并后进行。

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
