# DAgent

DAgent 是用 C++23 编写的终端 Agent，仅支持 Linux。前端 `dagent` 提供命令行与交互界面，并独占启动后端
`dagent-backend`，由后端负责配置、模型、工具与会话记录。

## 文档入口

| 目的 | 文档 |
| --- | --- |
| 构建、安装、准备运行环境 | [构建与安装](guide/build.md) |
| 配置模型、运行与恢复会话 | [使用指南](guide/usage.md) |
| 权限模式、审批、沙箱故障 | [权限与沙箱](guide/permissions.md) |
| 使用网页搜索、抓取与引擎配置 | [网页搜索与抓取](guide/web.md) |
| 理解当前架构和模块契约 | [设计索引](design/README.md) |
| 查看剩余工作与验收边界 | [后续工作](../nexttodo/README.md) |
| 查看选型背景和未落地建议 | [调研索引](research/README.md) |
| 查看已归档计划及有日期和范围的验收结论 | [历史记录](archive/README.md) |

`guide/` 说明当前使用方法，`design/` 描述当前实现，`nexttodo/` 跟踪剩余工作，`archive/` 保留历史计划与验收。
调研建议和历史快照不自动代表现有功能或当前运行状态。
本地 `temp/` 保存原始检测材料，不随仓库分发；关键结论在 `archive/` 保留摘要。

## 常用命令

```bash
dagent runtime sync        # 显式准备工具环境，不启动模型服务
dagent runtime list        # 配置与已准备环境是否匹配
dagent sandbox status      # 实际启动一次 SRT 隔离探测
dagent                     # 交互界面
dagent run "提示词"         # 非交互一轮；--output text|json|jsonl
dagent sessions            # 当前目录最近的会话
dagent --list-models
```

配置、提示词与主题位于 Home：dev 构建默认源码树 `home/`，其他构建默认可执行文件所在目录，
`DAGENT_HOME` 可覆盖。模型服务需要独立提供；配置地址不代表服务已启动。布局见 [home](design/home.md)。

## 构建

环境要求：

- Linux
- CMake ≥ 3.25、Ninja、支持 C++23 的编译器
- Boost ≥ 1.83、libcurl ≥ 7.68、OpenSSL Crypto
- 受限执行的宿主依赖：bubblewrap、socat，以及允许相应 namespace 的系统策略
- 网页工具的宿主依赖：curl 命令行程序 ≥ 7.75；HTML 解析库 lexbor v3.0.1 由 CMake 拉取
- 首次配置需要联网，CMake 会拉取其余第三方库

构建与安装步骤见 [构建与安装](guide/build.md)。

当前受限后端为 SRT 0.0.77；DAgent 的旧 Landlock/seccomp 后端及 libseccomp 构建依赖已移除。
`runtime sync` 准备 `internal/sandbox` 和 `internal/searxng` 等依赖；`sandbox status` 验证宿主隔离能力。
web_search/web_fetch 已实现，使用方式见 [网页指南](guide/web.md)，提交 `eec9829` 的本机验收见 [历史记录](archive/2026-10-05-web-tools-plan.md)。

开发与验证约束见仓库 [AGENTS.md](../AGENTS.md)：仅通过构建和真实功能运行确认程序修改，
临时检测材料放 `temp/`，不新增测试代码、模拟模型、演示入口或测试构建目标。
