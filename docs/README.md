# DAgent 文档

DAgent 是一个使用 C++23 和 CMake 构建的终端 Agent，仅支持 Linux。

## 当前状态

| 模块 | 位置 | 状态 |
| --- | --- | --- |
| TUI 框架 | `src/*/tui`，库 `dagent_tui` | 已完成并冻结（2026-09-18）：只修缺陷，不增删原语 |
| 应用层界面 | `src/*/ui`，库 `dagent_ui` | 起步：JSON 主题加载（`ui::load_theme`），默认主题 `config/themes/dagent.json` |
| Agent 运行时 | `src/*/agent`，库 `dagent_agent` | 进行中：LLM 编解码已完成（中立消息模型、OpenAI Chat Completions），agent 循环等其余部分未开始 |
| 基础库 | `src/*/base`，库 `dagent_base` | 已完成：日志、`.env` 密钥、文本工具、JSON 脱敏 |
| 子进程与沙箱 | `src/*/exec`，库 `dagent_exec` | 已完成：命令执行与进程组清理、长期子进程、bash 只读分析、Landlock + seccomp 沙箱 |
| 网络 | `src/*/net`，库 `dagent_net` | 已完成：libcurl 薄封装（整包/流式、stop_token 取消、超时分类）与 SSE 解析 |
| 工作区 | `src/*/workspace`，库 `dagent_workspace` | 已完成：文件原语（原子写入、stale 检测）、ripgrep 搜索与模糊匹配、unified diff、项目上下文（git、AGENTS.md、模板渲染） |
| 会话存储 | `src/*/session`，库 `dagent_session` | 已完成：JSONL 追加写入、UUIDv7、blob 外置、写入前脱敏、崩溃恢复、按项目过滤的 list |
| MCP 客户端 | `src/*/mcp`，库 `dagent_mcp` | 已完成：stdio 与 Streamable HTTP、现代（2026-07-28）与经典协议自动识别、取消/超时/断连、经典会话过期恢复；只做 tools |
| 工具层 | `src/*/tools`，库 `tools` | 已完成：read / write / edit / bash / grep / glob 与 MCP 工具包装；两阶段 prepare/run、Intent 供权限决策、FileTracker 做 stale 检测、按工具定义的 View |
| 入口层 | `src/*/app`，库 `dagent_app` | 已完成：分层配置加载与项目信任、用户级与项目级密钥、`.mcp.json`、命令行解析；可执行入口 `main` 待核心组装 |

`config/dagent.json` 是开发期配置（用 `--config` 显式指定），由 app 模块加载；配置文件、工作区与项目信任的约定见 [app 设计文档](design/app.md)。

## 后续工作

[next-to-do/](next-to-do/README.md)：外围模块的设计、技术路线与验收标准，共七个库——base（日志、密钥、
文本工具）、exec（子进程、沙箱）、workspace（文件、搜索、diff、项目上下文）、net（HTTP/SSE）、
session、mcp、app（配置、命令行），按里程碑 M1–M5 推进。模块完成并审核通过后，对应文档改写成 `design/`
下的设计文档，并从 next-to-do 删除。七个外围模块和 tools 层已全部完成，next-to-do 里保留的是统一约定与审核流程。

## 设计文档

设计文档描述**当前工作树的实际实现**，写给要使用或理解这个模块的人：它能做什么、怎样组织、怎样接入。
实现细节留在源码注释里，不写进设计文档。

| 文档 | 内容 |
| --- | --- |
| [base：日志与公共工具](design/base.md) | 日志接入与 `DAGENT_LOG`、`.env` 密钥与格式、文本工具与 JSON 脱敏的行为 |
| [net：HTTP 客户端与 SSE 解析](design/net.md) | 整包与流式请求、三种超时、错误分类、即时取消、连接复用与线程约束、SSE 解析规则 |
| [exec：子进程与沙箱](design/exec.md) | `run` 的行为与子进程运行环境、`Child`、只读判定白名单、沙箱模式与已知限制；exec 会让整个进程忽略 SIGPIPE |
| [workspace：文件、搜索、diff、项目上下文](design/workspace.md) | 路径解析与原子写入、ripgrep 调用与 fzy 模糊匹配、unified diff 的 hunk 合并、git 信息与 AGENTS.md 收集、inja 模板渲染 |
| [LLM 编解码：消息模型与厂商协议翻译](design/llm.md) | 中立消息模型与 StreamEvent、Codec 接口、OpenAI Chat Completions 的编解码规则、错误分类与重试、token 估算 |
| [session：会话存储](design/session.md) | 存储布局、Writer 的写入与恢复、崩溃后的截断与续写、list/replay 的边界、UUIDv7 |
| [app：配置与命令行](design/app.md) | 工作区根与项目根、配置文件布局与分层合并、项目信任、密钥优先级、`.mcp.json` 规则、命令行与入口约定 |
| [mcp：MCP 客户端](design/mcp.md) | 现代与经典协议的识别规则、stdio / Streamable HTTP 传输、请求头与 x-mcp-header、会话过期恢复、超时取消断连、错误分类与已知限制 |
| [tools：工具层](design/tools.md) | 两阶段 prepare/run 与核心的边界、参数与路径约定、FileTracker、各工具给模型的文本与报错、View 与会话序列化、对核心的要求 |
| [终端 UI 框架](design/tui-framework.md) | 框架能做什么、分层与对象关系、应用怎样接入、各模块的职责。源码注释中的 `§N` 指这份文档的章节 |

## 构建与测试

```bash
cmake --preset dev                 # 生成到 build/dev（Ninja，Debug）
cmake --build --preset dev
ctest --test-dir build/dev         # 运行 test/tui（tui_tests）
```

`test/tui` 用 15 个真实运行的用例覆盖 TUI 框架的能力，按设计文档的章节组织：

| 文件 | 覆盖 |
| --- | --- |
| `render_test.cpp` | 出帧差分（输出重放到虚拟终端后与网格一致）、字素切分与宽度（Unicode 官方测试数据）、容器布局、浮层摆放与补画 |
| `document_test.cpp` | 流式 Markdown 分块、块渲染器与主题令牌、滚动区锚点、选择与复制 |
| `runtime_test.cpp` | 界面模式的挂起与还原、能力握手、键盘解码与路由、post 不被慢帧阻塞、定时器与静止零唤醒、快捷键、鼠标选择复制到剪贴板 |

运行时用例在真实子进程里跑，经管道或 pty 注入按键、鼠标与终端应答。

依赖：CMake ≥ 3.25、支持 C++23 的编译器、Boost ≥ 1.83（Boost.Test）、libcurl。第三方头文件
随仓库放在 `src/public/lib/`（nlohmann/json v3.12.0、dtl）；需要源码构建的第三方库在 `cmake/deps.cmake`
中以 FetchContent 引入（spdlog v1.17.0、tree-sitter v0.27.0、tree-sitter-bash v0.25.1、inja v3.5.0、CLI11 v2.7.2），
首次配置需要联网；exec 的沙箱另需系统库 libseccomp（`apt install libseccomp-dev`）；workspace 的搜索与
项目上下文另需运行时程序 ripgrep 与 git。

## 目录约定

```
docs/
├── README.md    本索引
├── design/      设计文档：描述当前实现
└── next-to-do/  待实现模块的设计与技术路线（完成后迁入 design/）
```

- 目录名不含空格，避免 Markdown 链接需要转义。
- 设计文档描述现状，不描述目标能力，也不写实现细节。
- 代码改动后，同步更新对应的设计文档。
- 使用说明、问题清单、归档等目录在有内容时再建（`guide/`、`known-bugs/`、`archive/`），并在本索引登记。
- 遵循 [AGENTS.md](../AGENTS.md)：不创建模拟模型或演示入口；功能修改通过构建和真实运行验证，
  验证材料只放在 `temp/`。
