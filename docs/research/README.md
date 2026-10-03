# 调研与方案

本目录保存特定日期的源码/外部资料研究和设计判断，不是当前操作指南。
宿主状态、第三方服务、依赖版本与代码行号可能随时间变化；部署和实现以 [指南](../README.md) 与 [设计索引](../design/README.md) 为准。

| 文档 | 定位与当前关系 |
| --- | --- |
| [命令沙箱后端调研](command-sandbox-backend.md) | 2026-09-20 固定版本研究；当时未具备的 SRT 默认启动后来已完成，Landlock 备选未保留 |
| [沙箱修复方案比较](sandbox-repair-options.md) | 实施前选型背景；已选择专用 AppArmor profile + SRT |
| [Jev 权限审批研究](jev-permission-review.md) | 可选模型辅助审批建议；未集成 Jev，与确定性权限隔离分开 |
| [Linux 打包设计](linux-packaging.md) | 系统目录/发行包设计建议，未实现；现行目录安装见构建指南 |

验收结论见 [历史记录](../archive/README.md)，尚未实现的工作见 [计划索引](../../nexttodo/README.md)。
