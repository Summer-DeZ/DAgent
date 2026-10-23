# 设计文档

本目录描述 DAgent 的架构设计与关键流程。独立物理模块源码位于 `src/public/<模块>` 与 `src/private/<模块>`（其中 IPC 传输与协议客户端分别位于 `ipc` 与 `client` 子目录），跨模块专题（如 `home`、`skills`、`memory`、`toolchain`、`d3-authorization`、`opencode-theme`、`platform`）则横跨 `app`、`agent`、`tools` 与 `tui` 等模块实现。建议按「协议与前后端 → 会话控制 → Agent 核心循环」的顺序阅读前三篇。

使用与部署见 [文档入口](../README.md)，待办工作见 [nexttodo](../../nexttodo/README.md)，历史计划与归档见 [历史记录](../archive/README.md)。

| 文档 | 核心内容 |
| --- | --- |
| [protocol](protocol.md) | 前后端进程隔离、生命周期与私有 JSON-RPC 协议 |
| [runtime](runtime.md) | 会话状态机、输入队列、交互等待代理与子 Agent 调度 |
| [agent](agent.md) | 核心执行循环、工具并发调度、权限模型、上下文压缩与崩溃恢复 |
| [d3-authorization](d3-authorization.md) | d3 决策模型风险优先审阅、取证、审批链与动态阈值反馈 |
| [ui](ui.md) | 终端交互界面布局、数据流、外观设置与对话投影 |
| [opencode-theme](opencode-theme.md) | 主题配色体系、语义令牌与 256 色回退策略 |
| [tui-framework](tui-framework.md) | 终端 UI 框架分层、双缓冲渲染、文档流与输入路由 |
| [app](app.md) | 启动入口、命令行、配置体系、后端装配与非交互输出 |
| [home](home.md) | 自包含安装根目录布局、资源分层与指令优先级 |
| [skills](skills.md) | 全局技能发现、按轮激活与上下文生命周期 |
| [memory](memory.md) | 跨会话记忆存储、系统提示词索引注入与工具读写 |
| [toolchain](toolchain.md) | 托管工具链、独立依赖环境与不可变快照同步 |
| [tools](tools.md) | 工具层边界、两阶段预览授权、内置工具与视图投影 |
| [llm](llm.md) | 模型协议适配、流式累积、重试策略与设备登录 |
| [mcp](mcp.md) | MCP 客户端协议协商、沙箱 stdio 与 HTTP 传输 |
| [storage](storage.md) | SQLite 会话事件日志、跨进程写租约与决策审阅存储 |
| [workspace](workspace.md) | 防篡改文件原语、受限代码搜索、Diff 与项目环境采集 |
| [exec](exec.md) | 子进程管理、Shell 静态分析、MXC 沙箱隔离与回环网络代理 |
| [web](web.md) | 沙箱内网页抓取、HTML 转 Markdown、会话缓存与托管 SearXNG |
| [net](net.md) | HTTP 客户端、流式传输控制与 SSE 增量解析 |
| [base](base.md) | 日志体系、UTF-8 文本处理、路径规范化与公共基础原语 |
| [platform](platform.md) | 跨平台编译期分发边界与 Linux / Windows 实现策略 |
