# Jev 与自动模式权限审批

核查日期：2026-09-28。范围：源码与官方文档研究；未调用 Jev 服务，未验证准确率、延迟或本机当前沙箱能力。

下文源码行号与策略描述属于核查时点。后续权限链路已改为 SRT、显式请求范围及即时撤销，
当前契约见 [agent](../design/agent.md#7-权限与沙箱)。Jev 仍未集成；
父模型审批已独立实现，见 [权限指南](../guide/permissions.md) 与 [父 Agent 审批计划及验收归档](../archive/2026-10-03-parent-agent-approval-plan.md)。
该实现不使用 Jev，审阅失败时拒绝并反馈，不采用下文建议的人工回退。

## 结论

建议将 Jev 接为可选的语义审批层。确定性策略规定可自动审批的范围，Jev 仅在此范围内建议单次放行；不确定或服务失败时回到人工审批。不能把模型结果视为操作系统隔离或用户授权的替代品。

## 已核实事实

- 自动模式对应内部 `workspace`，界面名称为 `auto-edit`：[shell.cpp](../../src/private/ui/shell.cpp#L861)。
- `Policy::evaluate` 先处理危险命令、语法和只读限制，随后根据路径、命令及沙箱能力返回 allow/ask/deny：[permission.cpp](../../src/private/agent/permission.cpp#L174)。
- `workspace` 已自动允许普通工作区写入，以及工作区沙箱可用时的执行请求：[permission.cpp](../../src/private/agent/permission.cpp#L329)。因此，在剩余 ask 请求上接 Jev 的收益取决于实际请求构成。
- 工作区沙箱不可用时，非已知只读命令请求 `host_access`；批准会产生单次 `full_access` 执行授权：[请求](../../src/private/agent/permission.cpp#L292)、[授权](../../src/private/agent/permission.cpp#L341)。这描述代码分支，不代表本次已检查主机是否进入该分支。
- 调度器在 ask 分支调用独立的 `Approver`，批准后通过策略生成执行授权：[dispatch.cpp](../../src/private/agent/dispatch.cpp#L257)。当前 `Decision` 没有区分人工与模型决策的专用来源字段：[events.hpp](../../src/public/agent/events.hpp#L147)。
- Jev 接收 state 与 typed questions；Choice 返回 choice、probabilities、confidence，适合封闭选项判断：[官方介绍](https://docs.typesafe.ai/introduction)。
- confidence 是从输出概率分布计算的统计量；阈值需要结合具体任务表现确定：[官方 confidence 说明](https://docs.typesafe.ai/confidence)。类型正确不等于权限判断正确。
- 官方提供 HTTP POST 接口，可由 C++ 客户端集成：[Quick start](https://docs.typesafe.ai/introduction/quickstart)。

## 建议设计（未实现）

1. 保持 Policy 的确定性决策，不在纯策略层发网络请求。对可委托的 ask 请求，在现有人工 Approver 前组合一个模型审核服务。
2. 输入包含准确工具名、参数、规范化路径、命令分析、请求权限、沙箱能力，以及可信的用户授权上下文。仓库内容和工具输出只能作为待分析数据；不发送密钥或无关敏感内容。
3. 输出限定为“建议单次放行 / 需要人工确认”。程序根据许可范围与经实际使用校准的阈值生成 Decision；模型不生成会话永久授权。
4. 初版保留 host_access、敏感读取和受保护写入的人工审批。若希望模型处理宿主机提权，需要单独定义并由用户明确启用这种授权策略。
5. 模型超时、响应无效、上下文不足或不确定时交回人工；无人工入口则返回审批不可用，不执行。
6. 记录决策来源、模型版本、策略版本和概率，并将授权绑定到实际执行的工具、参数与权限范围，避免把模型批准记录成人工批准。

建议先在真实调用中旁路记录判断，由人工维持最终决定；依据误放行情况和实际减少的弹窗数，再决定自动批准范围。所有真实功能检测材料遵循项目规定放在 `temp/`，不增加测试代码或模拟入口。
