# 外围模块工作安排

这里是 agent 所有**外围模块**的设计与技术路线：每个模块做什么、不做什么、用哪个外部库、接口长什么样、
实现时要避开哪些坑、怎样验收。**核心 agent 工作流不在这里**，由你自己设计。

分工：你按本目录实现，每完成一部分就交给我审核。整个模块审核通过后，把对应文档从本目录删掉，改写成
`docs/design/` 下描述现状的设计文档（遵循 [docs/README.md](../README.md) 的约定）。

---

## 1. 边界：什么算外围

```
┌──────────────────────────── 你写：核心 ────────────────────────────┐
│ agent 循环 · 消息模型 · 权限决策 · 上下文压缩 · 重试策略 · 入口组装      │
│ 应用层界面（src/*/ui）                                               │
├──────────────────── tools 层（read/write/edit/bash…，待定）──────────┤
└──────────────────────────────┬─────────────────────────────────────┘
                               │ 调用
┌──────────────────────────────▼──── 外围（本目录）───────────────────┐
│ app · mcp · session · net · workspace · exec · base                 │
└─────────────────────────────────────────────────────────────────────┘
```

判断标准：**换一个完全不同的 agent 设计，这个模块还能原样复用吗？** 能复用的就是外围。比如「执行一条命令，
带超时并截断输出」是外围；「bash 工具返回给模型的文本长什么样」是 tools 层。tools 层的设计以后另行讨论。
LLM 编解码（中立消息模型与厂商协议之间的翻译）归核心，已完成，设计文档见 [docs/design/llm.md](../design/llm.md)。

---

## 2. 模块

七个库，从下往上排列：

| # | 模块 | 包含 | 外部依赖 | 状态 |
| --- | --- | --- | --- | --- |
| 01 | base（[设计文档](../design/base.md)） | 日志 · dotenv 与密钥 · 文本工具（UTF-8/截断/ANSI/base64）· JSON 脱敏 | spdlog | **已完成** |
| 02 | exec（[设计文档](../design/exec.md)） | 一次性命令 · 长期存活子进程 · 命令分析 · 沙箱 | Boost.Process v2 + Asio、libseccomp、tree-sitter-bash；内核 Landlock | **已完成** |
| 03 | workspace（[设计文档](../design/workspace.md)） | 文件原语 · 代码搜索 · diff · 项目上下文 | dtl、inja；运行时依赖 ripgrep、git | **已完成** |
| 04 | net（[设计文档](../design/net.md)） | HTTP · SSE | libcurl | **已完成** |
| 05 | session（[设计文档](../design/session.md)） | 会话存储（JSONL） | 暂时没有 | **已完成** |
| 06 | mcp（[设计文档](../design/mcp.md)） | MCP 客户端（stdio / Streamable HTTP，现代与经典协议） | 暂时没有 | **已完成** |
| 07 | [app](07-app.md) | 配置加载 · 命令行解析（入口层） | CLI11 | 未开始 |

### 依赖方向

```
app ──► 全部模块（它要认识所有模块的 Options）
mcp ──► exec, net
workspace ──► exec
exec, workspace, net, session, mcp ──► base
```

下层不知道上层，模块之间不形成环；外围不依赖核心，也不依赖 `tui` / `ui`。

### 里程碑

一个模块会分几次做完，每篇文档里都标明了各部分属于哪个里程碑。

| 里程碑 | 内容 | 做完之后能做什么 |
| --- | --- | --- |
| **M1 基础设施** | base 全部（已完成）· app（config、cli） | 你可以开始写 agent 循环：读配置、打日志、解析参数、用 net 调模型 |
| **M2 工具底座** | exec 的 process（已完成）· workspace 的 files、search、diff（已完成） | 能实现 bash/read/write/edit/grep/glob 工具，跑通第一个真正的编码任务 |
| **M3 持久化与上下文** | session（已完成）· workspace 的 context（已完成）；LLM 编解码归核心，已完成，见 [docs/design/llm.md](../design/llm.md) | 会话可以恢复；system prompt 带上项目信息 |
| **M4 扩展** | mcp（已完成）· exec 的 Child（已完成） | 接入外部 MCP 工具 |
| **M5 安全** | exec 的命令分析和沙箱（已完成） | bash 命令在受限环境里执行，只读命令自动放行 |

exec 已经整体完成（包括原计划放在 M4、M5 的 Child 和沙箱）；workspace 也已经整体完成（包括原计划放在
M3 的 context）。外围模块只剩 app（config、cli）未开始。

---

## 3. 统一约定

每个模块都遵守下面几条，审核时我会逐条对照。

### 3.1 文件与命名

- 头文件放 `src/public/<模块>/`，实现放 `src/private/<模块>/`（[AGENTS.md](../../AGENTS.md)）。模块内部按子功能拆成多个头文件，比如 `exec/process.hpp`、`exec/sandbox.hpp`。
- 每个模块一个静态库 `dagent_<模块>`，命名空间 `dagent::<模块>`。CMake 的写法照抄 [src/CMakeLists.txt](../../src/CMakeLists.txt) 里 `dagent_net` 那一段。
- 第三方库只在 `.cpp` 里 include，不出现在公开头文件里。例外有两个：nlohmann/json（它本身就是 JSON 的接口类型）和 `base/log.hpp` 里的 spdlog。

### 3.2 线程模型：阻塞调用 + stop_token + 回调

所有外围接口都和 `net::HttpClient` 保持同一种形状：

```cpp
Result do_something(const Input&, const Options&,
                    const std::function<void(Progress)>& on_progress = {},
                    std::stop_token stop = {});
```

- **在调用线程上阻塞执行**，回调也在调用线程上触发。开几个线程、在哪个线程上跑，由核心决定。
- **取消统一走 `std::stop_token`**，要求即时生效，不能靠轮询间隔。
- **外围永远不碰 TUI**。要更新界面，由核心在回调里调用 `Runtime::post()`。
- 模块内部可以用 Asio，但 `io_context` 是局部对象，不暴露到接口上。例外是 `exec::Child`：它是长期存活的，内部有自己的读取线程，文档里写明了回调在哪个线程上触发。

### 3.3 错误模型

- **传输或系统层面的失败抛异常**：每个模块一个 `XxxError : std::runtime_error`，带 `Kind` 枚举，参照 `net::HttpError`。`Kind` 按「调用方要不要换一种处理方式」来划分，不按错误码逐个列。
- **业务上的正常结果用返回值表达**：命令退出码非 0、HTTP 404、搜索没有结果、文件不存在，这些都不抛异常。
- 回调抛出的异常要原样传回调用方。如果回调是从 C 库里被调起的，先 catch 住存进 `std::exception_ptr`，出了 C 栈帧再 rethrow（net 里已经有这个写法）。
- 不写冗余的防御性检查（AGENTS.md），只处理真实可能发生、而且会造成静默错误的情况。

### 3.4 配置：模块只认 Options 结构

- 每个模块定义纯数据的 Options 结构，默认值和 `config/dagent.json` 保持一致。
- **模块自己不读配置文件**，JSON 到 Options 的映射统一在 app 里做。
- 时长用 `std::chrono` 类型，字节数用 `std::size_t`。

### 3.5 第三方依赖的引入方式

| 类别 | 方式 | 适用 |
| --- | --- | --- |
| 大型系统库 | `find_package`，用 apt 安装 | Boost、libcurl、libseccomp |
| 普通 C/C++ 库 | CMake `FetchContent`，**写死 release tag**，加 `FIND_PACKAGE_ARGS`（系统里有就用系统的） | CLI11、inja、tree-sitter |
| 需要特定编译选项的库 | `FetchContent`，**不加** `FIND_PACKAGE_ARGS`，始终从源码构建 | spdlog（要开 `SPDLOG_USE_STD_FORMAT`） |
| 小型、长期不更新的 header-only 库 | 放进 `src/public/lib/<库名>/`，保留 LICENSE | nlohmann/json（已有）、dtl |
| 运行时程序 | 启动时用 `exec::which` 检查，缺失时报错并给出安装提示 | ripgrep、git |

所有 `FetchContent_Declare` 集中写在 [cmake/deps.cmake](../../cmake/deps.cmake)（已由根 `CMakeLists.txt` include，目前有 spdlog、tree-sitter、tree-sitter-bash 与 libseccomp 的查找）。

需要你安装的系统包（Ubuntu 24.04）：

```bash
sudo apt install libseccomp-dev    # exec 的沙箱需要，已安装
# Boost 1.83、libcurl、ripgrep、git 本机已经有了
```

### 3.6 密钥

- 密钥只放在 `.env.dev` 或 `.env` 里（格式见 [.env.example](../../.env.example)），通过 `base::Secrets` 读取，**不写进进程环境变量**。
- `exec` 默认把名字像密钥的环境变量从子进程里过滤掉。
- 任何日志、异常信息、会话记录里都不能出现密钥。

### 3.7 验收

- 构建通过，`-Wall -Wextra` 零警告。
- 在 `temp/<模块>_check/` 下写真实运行的检测程序（AGENTS.md：不做模拟，不做冒烟测试）。每篇文档的「验收」一节列出了必须跑通的场景。
- 需要模型的场景：可以用本机 `127.0.0.1:10000` 上的 `qwen3.6-35b-a3b`（懒加载，冷启动约 3 分钟，而且**会丢掉 `tools` 字段**），也可以用 DeepSeek（key 在 `.env.dev` 里）。

---

## 4. 审核流程

1. 你实现完一部分以后，告诉我审核的范围，比如「审核 exec process」。
2. 我对照文档检查三类问题：**接口是否符合约定**、**文档里列出的坑是否都处理了**、**正确性缺陷**。结果按严重程度排序给你。
3. 我会亲自跑一遍 `temp/` 下的检测程序，并补上文档里有、检测程序没覆盖到的场景。
4. 整个模块审核通过后，写 `docs/design/<模块>.md`，从本目录删掉对应文档，并更新上面的状态表和 [docs/README.md](../README.md)。

如果实现时发现文档里的设计不合理，直接改，审核时说明理由就行。文档是起点，不是合同。
