# SRT 与权限链路验收记录

代码基线：`c923033`（SRT 与文件权限整合）、`2a06679`（权限生命周期、旧后端移除及默认启动修复）。
本页是已有真实运行记录的摘要，不表示阅读本文时主机或模型服务仍处于相同状态。

## 2026-09-29

当日 S06–S10 记录覆盖了活跃执行撤销、权限切换、旧审批重判、父权限收窄、MCP profile 与连接失效、
HTTP 远端边界、审批载荷、审计记录和 TUI。受限执行使用 `aa-exec -p linux-sandbox`，
普通启动仍失败，因此不能将这些结果解释为默认主机部署已经完成。

旧 Landlock/seccomp 执行后端和 libseccomp 构建依赖在此次切换中移除；受限后端统一为 SRT。
原始本地材料位于 `temp/s05-repair/VERIFY.md` 与 `temp/s06-s10-driver/VERIFY.md`，未纳入版本控制。
下述 9 月 30 日验证没有重新覆盖所有 MCP 和网络场景。

## 2026-09-30

凌晨三个 explore 子任务的 glob/grep 因 SRT 不可用失败，随后耗尽工具额度；父任务未正确接收 limit 状态。
修复后部署了匹配开发后端路径的专用 AppArmor profile，保持全局 userns 限制开启。
继续实际并发运行时发现控制 FD 继承缺失，修复为显式白名单与 SOCK_CLOEXEC。

| 项目 | 实际结果 |
| --- | --- |
| 构建 | dev 的 dagent、dagent-backend 构建通过 |
| 默认启动 | 无 aa-exec 的 sandbox status 返回 probe.ok=true、sandboxing_enabled=true、SRT 0.0.77 |
| 配置前置 | 当前后端路径匹配专用 profile；apparmor_restrict_unprivileged_userns 仍为 1 |
| 三子 Agent 并发 | 父 read_only；三个 explore 各执行 10、6、5 次工具调用，共 21 次，零工具错误，全部 done |
| 调用额度 | implement 在 1 次 read 后达到 limit；父 task 的 is_error=true，输出明确标记未完成部分 |
| 父取消 | 子任务在 SRT 中运行 sleep 45，取消父 run 后约 0.052 秒回传 interrupted，父回合也 interrupted |
| 父降权 | workspace→unrestricted→ask 后约 0.053 秒回传子任务 interrupted，父随后正常总结 |
| 目录安装资源 | 临时目录安装成功，生成的 AppArmor profile 匹配最终安装路径；未加载该临时安装 profile |

原始记录定位：

| 场景 | 会话 ID |
| --- | --- |
| 并发搜索 | `01a0f11e-6669-7f57-b46c-fd7bdcce56c5` |
| 额度耗尽 | `01a0f119-6f0c-70f1-8dfb-293f23a2eb39` |
| 父取消 | `01a0f11f-21b9-7945-b3e9-3283c906aa61` |
| 父降权 | `01a0f11f-4f7d-72bc-b857-e837ea837c89` |

详细本地材料位于 `temp/srt-default-start-20260930/`，会话保存在当时的 Home 数据库中。
验证使用的 Qwen 模型服务随后按用户要求停止；本记录不表示该服务应常驻或已经启动。

## 未覆盖范围

- 完整并发对抗矩阵，包括多子任务退出、权限变更与授权撤销的所有交错。
- 所有 MCP 传输、网络与隔离场景在最终二进制上的重复验收。
- 完整发行安装根、升级/卸载及跨主机、跨路径运行验收。
- 父模型自动审批子 Agent；该功能仍处于独立设计阶段。

部署以 [构建与安装](../guide/build.md) 为准，后续工作见 [计划索引](../../nexttodo/README.md)。
