# 后续工作

更新：2026-10-22。

进行中：

| 计划 | 状态 | 说明 |
| --- | --- | --- |
| [Agent 记忆](memory-plan.md) | 已完成（M1–M5、E1） | 全局与项目两层记忆、memory 工具、索引注入、`/memory` 面板；MemoryCode 与自定义场景评估见 [报告](../docs/report/memory-evaluation.md) |
| [模型登录与请求头](model-auth-plan.md) | 已完成（TUI 改版第 3 期） | `/model` 中登录 OpenCode Go 并选择模型、设备授权与自动刷新、通用模型请求头 |
| [agent / tui / ui 目录整理](library-layout-plan.md) | 已完成（两阶段，2026-10-22） | 三个最大的库按职责分子目录；shell、transcript、markdown、permission、authorization、grapheme 按职责拆分 |
| [TUI 改版](tui-redesign-plan.md) | 已完成（第 1–5 期） | 标题栏与提示栏、分节信息栏、`/settings` 外观自定义（背景模式、强调色、主题预设）、新组件；第 3 期合并模型登录 |

验收只使用构建和真实功能运行；临时材料放 `temp/`，不增加测试代码、模拟模型、模拟服务或测试目标。
