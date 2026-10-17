# 后续工作

更新：2026-10-17。此前的 SRT、父 Agent 审批和 web 工具计划及其剩余验收均已完成，记录见
[历史归档](../docs/archive/README.md)。

待实现：

| 计划 | 状态 | 说明 |
| --- | --- | --- |
| [d3 权限审批](d3-approval-plan.md) | 设计完成，待实现 | auto-edit 引入 d3 审阅与代批；unrestricted 不引入 d3，新增开启警告 |

验收只使用构建和真实功能运行；临时材料放 `temp/`，不增加测试代码、模拟模型、模拟服务或测试目标。
每台新主机仍需执行 `runtime sync` 和 `sandbox status`，历史成功记录不能替代当前探测。
使用方法见 [文档入口](../docs/README.md)；未采纳的方案建议保留在 [调研索引](../docs/research/README.md)。
