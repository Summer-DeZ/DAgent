# DAgent 文档

DAgent 是一个使用 C++23 和 CMake 构建的终端 Agent，仅支持 Linux。

## 当前状态

| 模块 | 位置 | 状态 |
| --- | --- | --- |
| TUI 框架 | `src/*/tui`，库 `dagent_tui` | 已完成并冻结（2026-09-18）：只修缺陷，不增删原语 |
| 应用层界面 | `src/*/ui`，库 `dagent_ui` | 起步：JSON 主题加载（`ui::load_theme`），默认主题 `config/themes/dagent.json` |
| Agent 运行时 | `src/*/agent` | 未开始 |
| 网络 | `src/*/net` | 未开始 |

`config/dagent.json` 已包含模型网关、HTTP、上下文、会话等配置项，目前还没有代码读取它。

## 设计文档

设计文档描述**当前工作树的实际实现**；未实现或有限制的地方列在各文档的「已知缺口」小节。

| 文档 | 内容 |
| --- | --- |
| [终端 UI 框架](design/tui-framework.md) | 分层、线程模型、渲染管线、字素与宽度、布局与浮层、文档层与流式 Markdown、输入、运行时、主题、应用层约定。源码注释中的 `§N` 指这份文档的章节 |

## 构建与测试

```bash
cmake --preset dev                 # 生成到 build/dev（Ninja，Debug）
cmake --build --preset dev
ctest --test-dir build/dev         # 运行 test/tui（tui_tests）
```

依赖：CMake ≥ 3.25、支持 C++23 的编译器、Boost ≥ 1.83（Boost.Test）、libcurl。第三方头文件
随仓库放在 `src/public/lib/`（nlohmann/json v3.12.0）。

## 目录约定

```
docs/
├── README.md    本索引
└── design/      设计文档：描述当前实现
```

- 目录名不含空格，避免 Markdown 链接需要转义。
- 设计文档描述现状，不描述目标能力；未实现的能力在对应文档的「已知缺口」小节列出，不混入正文。
- 代码改动后，同步更新对应的设计文档。
- 使用说明、问题清单、归档等目录在有内容时再建（`guide/`、`known-bugs/`、`archive/`），并在本索引登记。
- 遵循 [AGENTS.md](../AGENTS.md)：不创建模拟模型或演示入口；功能修改通过构建和真实运行验证，
  验证材料只放在 `temp/`。
