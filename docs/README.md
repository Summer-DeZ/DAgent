# DAgent 文档

DAgent 是一个使用 C++23 和 CMake 构建的终端 Agent，仅支持 Linux。

## 当前状态

DAgent 由两个正式进程组成：前端 `dagent`（命令行、终端界面、run 输出）与它独占启动的 `dagent-backend`
（配置、模型、工具、会话记录）。两者经私有 socketpair 上的 JSON-RPC 通信；前端不链接执行与存储实现，后端不链接 UI。

| 模块 | 位置 | 状态 |
| --- | --- | --- |
| TUI 框架 | `src/*/tui`，库 `dagent_tui` | 已完成并冻结（2026-09-18）：只修缺陷，不增删原语 |
| 应用层界面 | `src/*/ui`，库 `dagent_ui` | 已完成：只持协议客户端与页面状态；居中对话与工具卡片、右侧计划栏、输入/文件补全、命令/会话/模型/主题面板、toast、权限对话框、状态栏与运行时主题切换 |
| 核心业务 | `src/*/agent`，库 `dagent_agent` | 已完成：Session / Run / TurnRunner / ActionDispatcher、控制动作、三档权限与只读/plan、上下文压缩、记录编解码/恢复/历史投影；只依赖 base |
| 会话控制 | `src/*/runtime`，库 `dagent_runtime` | 已完成：SessionController（输入队列、new/resume/切模型/压缩）、InteractionBroker、SubagentExecutor |
| 模型客户端 | `src/*/llm`，库 `dagent_llm` | 已完成：openai-chat / ollama / anthropic 编解码、流式累积与重试 |
| 私有协议 | `src/*/{protocol,ipc,client,backend}` | 已完成：JSON-RPC DTO、socketpair 分帧与后端进程启动、前端客户端、后端方法分发与有序发布 |
| 基础库 | `src/*/base`，库 `dagent_base` | 已完成：日志、通用 dotenv 解析、文本工具、JSON 脱敏；app 不读取 `.env` |
| 子进程与沙箱 | `src/*/exec`，库 `dagent_exec` | 已完成：命令执行与进程组清理、长期子进程、bash 只读分析、Landlock + seccomp 沙箱 |
| 网络 | `src/*/net`，库 `dagent_net` | 已完成：libcurl 薄封装（整包/流式、stop_token 取消、超时分类）与 SSE 解析 |
| 工作区 | `src/*/workspace`，库 `dagent_workspace` | 已完成：文件原语（原子写入、stale 检测）、ripgrep 搜索与模糊匹配、unified diff、项目上下文（git、AGENTS.md、模板渲染） |
| 会话存储 | `src/*/storage`，库 `dagent_storage` | 已完成：单文件 SQLite、UUIDv7、写入前脱敏、事务化崩溃标记、按 cwd 索引、跨进程会话写锁、只读分页 |
| MCP 客户端 | `src/*/mcp`，库 `dagent_mcp` | 已完成：stdio 与 Streamable HTTP、现代（2026-07-28）与经典协议自动识别、取消/超时/断连、经典会话过期恢复；只做 tools |
| 工具层 | `src/*/tools`，库 `tools` | 已完成：read / write / edit / bash / grep / glob 与 MCP 工具包装、McpHub；两阶段 prepare/execute、中立意图、FileTracker |
| 入口与装配 | `src/*/app`，库 `dagent_app_cli` / `dagent_app_config` | 已完成：命令行、安装根、后端启动与 run 输出；后端配置读取与会话装配 |

dev 构建默认从源码树的 `home/` 读取配置、提示词和主题；显式 `DAGENT_HOME` 可覆盖。其它构建默认读取可执行
文件所在目录。安装布局与命令行约定见 [app 设计文档](design/app.md)。
当前开发网关为本机 `http://127.0.0.1:10009/v1` 的 Qwen3.8-Flash-Next，无需密钥；通过
`chat_template_kwargs.enable_thinking=true` 打开思考输出，界面折成 `+ Thought` 一行（ctrl+r 展开）。
思考 token 走输出预算，所以 `max_tokens` 设为 8192。服务需先启动。
本地服务预填充十几万 token 时可能几分钟不返回字节，超过 `http.idle_timeout_seconds`（120 秒）会按超时失败；
做超长上下文实验时临时加 `--set http.idle_timeout_seconds=900`，不改默认配置。

## 阅读顺序

先读 [protocol：前后端进程与私有协议](design/protocol.md) 了解两个进程怎样分工，再读 [runtime：会话控制](design/runtime.md)
与 [agent：核心业务对象](design/agent.md) 了解一轮的数据流、记录与恢复，然后按关注点读 [ui](design/ui.md)、
[app](design/app.md)、[tools](design/tools.md)。底层协议、存储、执行与终端原语分别由下表的模块文档说明。

整体等价重构（前后端分离、核心对象拆分）已按 [执行规格](next-to-do/README.md) 完成 R01–R14 并真实验收；
该目录保留规格与各任务的验收记录，当前能力及限制以 `design/` 和源码为准。

待实施计划：[命令分析、权限与沙箱整体改造](../nexttodo/command-permissions-plan.md)。
该计划描述目标行为、P01–P10 实施依赖与真实运行验收，不代表当前已经实现的能力。

## 设计文档

设计文档描述**当前工作树的实际实现**，写给要使用或理解这个模块的人：它能做什么、怎样组织、怎样接入。
实现细节留在源码注释里，不写进设计文档。

| 文档 | 内容 |
| --- | --- |
| [protocol：前后端进程与私有协议](design/protocol.md) | 进程启动与关闭、传输、身份与版本、请求方法、事件与快照顺序、背压、错误码、前端三种使用方式 |
| [runtime：会话控制、交互与子执行](design/runtime.md) | 对象与装配端口、会话控制状态机、替换会话、快照发布、交互代理、子执行、线程与关闭 |
| [agent：核心业务对象与执行循环](design/agent.md) | 对象与端口、线程与取消、Event、历史不变式、循环与调度、权限与控制动作、上下文、提示词、记录/恢复/历史投影、MCP 生命周期、子 Agent |
| [ui：应用层交互界面](design/ui.md) | 控件树、主题、线程与 RPC、输入与补全、对话投影与子 Pane、状态与权限浮层、退出、模型切换 |
| [base：日志与公共工具](design/base.md) | 日志接入与 `DAGENT_LOG`、通用 dotenv 解析、文本工具与 JSON 脱敏的行为 |
| [net：HTTP 客户端与 SSE 解析](design/net.md) | 整包与流式请求、三种超时、错误分类、即时取消、连接复用与线程约束、SSE 解析规则 |
| [exec：子进程与沙箱](design/exec.md) | `run` 的行为与子进程运行环境、`Child`、只读判定白名单、沙箱模式与已知限制；exec 会让整个进程忽略 SIGPIPE |
| [workspace：文件、搜索、diff、项目上下文](design/workspace.md) | 路径解析与原子写入、ripgrep 调用与 fzy 模糊匹配、unified diff 的 hunk 合并、git 信息与 AGENTS.md 收集、inja 模板渲染 |
| [llm：模型客户端与 Provider](design/llm.md) | ProviderConfig 与公开描述、Codec、各 provider 编解码、错误分类与预算、流式累积与重试 |
| [storage：会话存储](design/storage.md) | 存储端口实现、schema 与迁移、写入事务与崩溃、跨进程写锁、只读列表与历史分页、库损坏处理 |
| [app：配置与命令行](design/app.md) | 两个入口与库、自包含安装根、配置与提示词、模型添加与写锁、命令行、后端装配、run 输出、信号与退出码 |
| [mcp：MCP 客户端](design/mcp.md) | 现代与经典协议的识别规则、stdio / Streamable HTTP 传输、请求头与 x-mcp-header、会话过期恢复、超时取消断连、错误分类与已知限制 |
| [tools：工具层](design/tools.md) | 两阶段 prepare/run 与核心的边界、参数与路径约定、FileTracker、各工具给模型的文本与报错、View 与会话序列化、对核心的要求 |
| [终端 UI 框架](design/tui-framework.md) | 框架能做什么、分层与对象关系、应用怎样接入、各模块的职责。源码注释中的 `§N` 指这份文档的章节 |

## 构建、安装与运行

```bash
cmake --preset dev                                        # 生成到 build/dev（Ninja，Debug）
cmake --build --preset dev --target dagent dagent-backend # 两个正式可执行文件，输出在 build/dev/src/
build/dev/src/dagent                                      # 交互界面（dev 构建默认安装根为源码树 home/）
build/dev/src/dagent run "提示词"                          # 非交互一轮；--output text|json|jsonl
build/dev/src/dagent sessions                             # 当前目录最近的会话
build/dev/src/dagent --list-models
```

`dagent` 必须与 `dagent-backend` 位于同一目录：前端从自身目录启动后端，找不到时启动失败。
正式安装使用非 dev 构建，安装根即可执行文件所在目录（也可用 `DAGENT_HOME` 指定）：

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target dagent dagent-backend
cmake --install build/release --prefix <安装根>   # 两个可执行文件、config.json、models.json(0600)、提示词、themes/、agents/
```

测试只保留冻结 TUI 框架的真实用例：

```bash
cmake --build --preset dev && ctest --test-dir build/dev   # 运行 test/tui（tui_tests）
```

`test/tui` 用 15 个真实运行的用例覆盖 TUI 框架的能力，按设计文档的章节组织：

| 文件 | 覆盖 |
| --- | --- |
| `render_test.cpp` | 出帧差分（输出重放到虚拟终端后与网格一致）、字素切分与宽度（Unicode 官方测试数据）、容器布局、浮层摆放与补画 |
| `document_test.cpp` | 流式 Markdown 分块、块渲染器与主题令牌、滚动区锚点、选择与复制 |
| `runtime_test.cpp` | 界面模式的挂起与还原、能力握手、键盘解码与路由、post 不被慢帧阻塞、定时器与静止零唤醒、快捷键、鼠标选择复制到剪贴板 |

运行时用例在真实子进程里跑，经管道或 pty 注入按键、鼠标与终端应答。

依赖：CMake ≥ 3.25、支持 C++23 的编译器、Boost ≥ 1.83（Boost.Test）、libcurl。第三方头文件
随仓库放在 `src/public/lib/`（nlohmann/json v3.12.0、dtl、SQLite amalgamation）；需要源码构建的第三方库在 `cmake/deps.cmake`
中以 FetchContent 引入（spdlog v1.17.0、tree-sitter v0.27.0、tree-sitter-bash v0.25.1、inja v3.5.0、CLI11 v2.7.2），
首次配置需要联网；exec 的沙箱另需系统库 libseccomp（`apt install libseccomp-dev`）；workspace 的搜索与
项目上下文另需运行时程序 ripgrep 与 git。

### 临时检测程序

AGENTS.md 不允许为测试加构建目标，真实功能检测都是放在 `temp/` 下、在 CMake 之外链接的临时程序。`temp/` 随时可能
清空，实现、持久化测试和文档都不能依赖它。链接写法（先 `cmake --build --preset dev`，库按依赖从上到下排列，用到
哪些就链哪些）：

```bash
b=build/dev
g++ -std=c++23 -Wall -Wextra -DSPDLOG_COMPILED_LIB -DSPDLOG_USE_STD_FORMAT \
    -I src/public -I $b/_deps/spdlog-src/include temp/check.cc \
    $b/src/libdagent_app_config.a $b/src/libdagent_runtime.a $b/src/libtools.a $b/src/libdagent_llm.a \
    $b/src/libdagent_storage.a $b/src/libdagent_agent.a $b/src/libdagent_mcp.a $b/src/libdagent_workspace.a \
    $b/src/libdagent_net.a $b/src/libdagent_exec.a \
    $b/src/libdagent_base.a $b/_deps/spdlog-build/libspdlogd.a \
    $b/_deps/tree-sitter-build/libtree-sitter.a $b/libtree-sitter-bash.a \
    -lcurl -lseccomp -pthread -o temp/check
```

## 目录约定

```
docs/
├── README.md      本索引
├── design/        架构与模块设计：描述当前实现
└── next-to-do/    已完成的等价重构规格与真实验收记录
```

- 目录名不含空格，避免 Markdown 链接需要转义。
- 设计文档描述现状，不描述目标能力，也不写实现细节。
- 代码改动后，同步更新对应的设计文档。
- 使用说明、问题清单、归档等目录在有内容时再建（`guide/`、`known-bugs/`、`archive/`），并在本索引登记。
- 遵循 [AGENTS.md](../AGENTS.md)：不创建模拟模型或演示入口；功能修改通过构建和真实运行验证。
  `temp/` 只放临时的验证材料，随时可能清空；实现、持久化测试和文档都不能依赖其中的内容。
