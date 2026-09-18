# DAgent 文档

DAgent 是一个使用 C++23 和 CMake 构建的终端 Agent，仅支持 Linux。

## 当前状态

| 模块 | 位置 | 状态 |
| --- | --- | --- |
| TUI 框架 | `src/*/tui`，库 `dagent_tui` | 已完成并冻结（2026-09-18）：只修缺陷，不增删原语 |
| 应用层界面 | `src/*/ui`，库 `dagent_ui` | 起步：JSON 主题加载（`ui::load_theme`），默认主题 `config/themes/dagent.json` |
| Agent 运行时 | `src/*/agent` | 未开始 |
| 基础库 | `src/*/base`，库 `dagent_base` | 已完成：日志、`.env` 密钥、文本工具、JSON 脱敏 |
| 子进程与沙箱 | `src/*/exec`，库 `dagent_exec` | 已完成：命令执行与进程组清理、长期子进程、bash 只读分析、Landlock + seccomp 沙箱 |
| 网络 | `src/*/net`，库 `dagent_net` | 已完成：libcurl 薄封装（整包/流式、stop_token 取消、超时分类）与 SSE 解析；设计文档待补 |

`config/dagent.json` 已包含模型网关、HTTP、上下文、会话等配置项，目前还没有代码读取它。

## 后续工作

[next-to-do/](next-to-do/README.md)：外围模块的设计、技术路线与验收标准，共七个库——base（日志、密钥、
文本工具）、exec（子进程、沙箱）、workspace（文件、搜索、diff、项目上下文）、net（HTTP/SSE 已完成，LLM 编解码）、
session、mcp、app（配置、命令行），按里程碑 M1–M5 推进。模块完成并审核通过后，对应文档改写成 `design/`
下的设计文档，并从 next-to-do 删除。

## 设计文档

设计文档描述**当前工作树的实际实现**，写给要使用或理解这个模块的人：它能做什么、怎样组织、怎样接入。
实现细节留在源码注释里，不写进设计文档。

| 文档 | 内容 |
| --- | --- |
| [base：日志与公共工具](design/base.md) | 日志接入与 `DAGENT_LOG`、`.env` 密钥与格式、文本工具与 JSON 脱敏的行为 |
| [exec：子进程与沙箱](design/exec.md) | `run` 的行为与子进程运行环境、`Child`、只读判定白名单、沙箱模式与已知限制；exec 会让整个进程忽略 SIGPIPE |
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
随仓库放在 `src/public/lib/`（nlohmann/json v3.12.0）；需要源码构建的第三方库在 `cmake/deps.cmake`
中以 FetchContent 引入（spdlog v1.17.0、tree-sitter v0.27.0、tree-sitter-bash v0.25.1），首次配置需要联网；
exec 的沙箱另需系统库 libseccomp（`apt install libseccomp-dev`）。

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
