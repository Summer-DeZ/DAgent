# 设计文档

描述当前实现。各模块代码在 `src/*/<模块>`，建议先读前三篇。

使用和部署从 [文档入口](../README.md) 开始；剩余工作见 [nexttodo](../../nexttodo/README.md)，
已归档计划与验收范围见 [历史记录](../archive/README.md)。本文档中的设计能力不等于每台主机已经具备运行前置。

| 文档 | 内容 |
| --- | --- |
| [protocol](protocol.md) | 前后端进程与私有协议 |
| [runtime](runtime.md) | 会话控制、交互代理、子执行 |
| [agent](agent.md) | 核心业务对象与执行循环、权限、上下文、记录与恢复 |
| [ui](ui.md) | 应用层界面 |
| [opencode-theme](opencode-theme.md) | 当前主题结构、预览与终端配色 |
| [tui-framework](tui-framework.md) | 终端 UI 框架 |
| [app](app.md) | 入口、命令行、配置、后端装配、run 输出 |
| [home](home.md) | 安装根的目录布局、配置与资源 |
| [skills](skills.md) | 全局技能 |
| [toolchain](toolchain.md) | 托管工具与依赖 |
| [tools](tools.md) | 内置工具与 MCP 工具包装 |
| [llm](llm.md) | 模型客户端（openai-chat / ollama / anthropic） |
| [mcp](mcp.md) | MCP 客户端 |
| [storage](storage.md) | SQLite 会话存储 |
| [workspace](workspace.md) | 文件原语、搜索、diff、项目上下文 |
| [exec](exec.md) | 子进程与沙箱 |
| [net](net.md) | HTTP 客户端与 SSE 解析 |
| [base](base.md) | 日志与公共工具 |
