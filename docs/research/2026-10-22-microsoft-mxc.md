# Microsoft Execution Containers（MXC）调研

日期：2026-10-22。目的：评估 DAgent Windows 移植第三步（沙箱后端）的技术路线。
本文只记录调研事实与建议，不构成已批准的计划。

## 1. MXC 是什么

MXC（Microsoft Execution Containers）是微软在 Build 2026（2026-06-02）公布、2026-10-07 宣布 GA 的
"面向 AI Agent 与不可信代码的策略驱动执行层"。应用用一份声明式 JSON 描述工作负载需要的资源
（文件、网络目的地、UI），MXC 选择后端并由操作系统在运行时强制边界；策略不在工作负载的控制之下。

| 项目 | 事实 |
| --- | --- |
| 仓库 | github.com/microsoft/mxc，MIT 许可，Rust 实现，约 2.9k star，持续活跃（2026-10-22 仍有提交） |
| 版本 | SDK v1.0.0（2026-10-07）、v1.0.1（2026-10-17）；0.x → 1.0 几乎重命名了全部 API |
| SDK | Node `@microsoft/mxc-sdk`（Node ≥ 24，解包约 108 MB，依赖 koffi 与 node-pty）；.NET `Microsoft.Mxc.Sdk`；Rust `mxc-sdk` |
| 原生接口 | `src/ffi/mxc_ffi`：cdylib/staticlib 形式的 C ABI（`mxc_ffi.dll`），输入输出均为精确版本号的 JSON：`mxc_run_json`、`mxc_spawn_json`（流式句柄）、`mxc_probe_request_json(_with_error)`、`mxc_available_backends_json`、`mxc_platform_support_json`、状态化生命周期入口。.NET SDK 就是通过它 P/Invoke 的；Node SDK 经 koffi 加载同一库 |
| 执行器二进制 | `wxc-exec.exe`（接受 JSON 请求，`--probe` 只做探测）、`wxc-host-prep.exe`（需管理员，只服务 AppContainer 回退层） |
| 发布物 | GitHub Release 附件 `mxc-release-binaries.zip`（约 542 MB，含各平台原生资产）；SDK 包内自带原生库，应用不必单独启动执行器进程 |

## 2. 后端矩阵

| 平台 | 默认后端 | 其他后端 |
| --- | --- | --- |
| Windows 11 x64/ARM64 | `processcontainer` | `isolation_session`、`wslc`、实验性：`windows_sandbox`、`microvm`、`hyperlight` |
| Linux x64/ARM64 | `bubblewrap` | `lxc`、`microvm`、`hyperlight` |
| macOS | `seatbelt` | 无 |

策略语言 1.0.0 的顶层字段：`version`（必须为 `"1.0.0"`）、`process`（`commandLine`、`cwd`、`timeout`、`env`、`inheritDefaultEnv`）、
`filesystem`（`readwritePaths` / `readonlyPaths` / `deniedPaths`，未列出的路径一律不可访问，cwd 不隐含授权）、
`network`（`egress` 默认 deny，`allow`/`deny` 规则按 CIDR、协议、端口，deny 优先；`ingress.default` 与 `ingress.hostLoopback` 各自默认 deny）、
`ui`（`disable` 默认 true、`clipboard`、`injection`）、`containment`（后端选择）、`runtimeConfig.networkProxy`（调用方自管的回环代理）、
`fallback.allowDaclMutation`（设为 false 可拒绝 Windows 的 DACL 回退层）、`lifecycle`、`telemetry` 以及各后端专属段。

## 3. Windows ProcessContainer 的分层

ProcessContainer 在运行时按宿主能力选择最强可用的进程创建 API，这是理解 MXC 安全承诺的关键：

| 层 | 机制 | 要求 | 网络强制 |
| --- | --- | --- | --- |
| BaseContainer（PSEC） | `CreateProcessSecurityEnvironment` / `Experimental_CreateProcessInSandbox`（`processmodel.dll`），策略由 OS 在其特权上下文中应用并持有到工作负载结束；每次启动无 UAC | Windows 11 24H2 ≥ 26100.9278 或 25H2 ≥ 26200.9278（2026-08 累积更新 KB5120998）；容器内监听回环需 ≥ 26x00.9550（2026-09） | 按容器 SID 的 WFP 出站过滤（IP/CIDR/端口方向规则）、每容器 WinHTTP 代理设置、回环代理身份绑定 |
| AppContainer + BFS / AppContainer + DACL | 传统 AppContainer | 任意 Windows 11；DACL 层需要管理员运行 `wxc-host-prep prepare-system-drive`，且 `\Device\Null` 每次开机后需 `prepare-null-device` | 只有 capability 粒度（`internetClient`、`privateNetworkClientServer`），不安装 WFP 规则；显式 egress 规则、代理身份、hostLoopback allow 会被类型化拒绝 |

`deniedPaths` 在 BaseContainer 上需要宿主报告 `PSE_SUPPORT_FS_DENY` 能力（Codex 与 SRT 的接入都按能力探测，不按 OS 版本号判断）。
BaseContainer 没有"只拒绝写"的原语，`denyWrite` 语义可能过度拒绝（SRT 集成 PR 的未决项）。

网络有三种模型：直连 + WFP 过滤（规则是 IP 级，不是域名）；代理模式（推荐，`runtimeConfig.networkProxy` 必须是回环地址，
MXC 设置 `HTTP_PROXY`/`HTTPS_PROXY` 与 WinHTTP 代理，目的地过滤由代理负责；无身份代理要求 `ingress.hostLoopback: "allow"`）；
完全封闭。域名级 allowlist 必须由调用方自己的代理实现。

运行模式：Enforcement（强制）、Learning（阻止并记录 JSON 活动报告）、Permissive（放行并记录）；活动报告目前仅 Windows 进程容器提供。
Windows 上的遥测需要用户在 MXC 的同意机制里明确同意，管理员策略可禁止但不能代为同意。

## 4. 生态采用情况

| 项目 | 做法 |
| --- | --- |
| GitHub Copilot CLI | Windows 上使用 ProcessContainer 的 BaseContainer 层；shell 命令、本地 MCP server、语言服务器默认在容器内运行；内置文件工具在进程内按策略检查，不是 OS 强制 |
| OpenAI Codex | `codex-rs/mxc-sandbox` crate 用 Rust 直接调用 MXC 的 `BaseContainerRunner`，明确"从不使用 AppContainer 分发器、不改宿主 ACL、不建沙箱用户、不请求提权"；可用性用 MXC 的 create/close 探针而不是 OS 版本判断；默认关闭的 `features.prefer_mxc` 控制灰度；自研的 `windows-sandbox-rs`（受限 token + 服务）保留为旧后端；命令失败不回退后端 |
| Anthropic sandbox-runtime（SRT） | Windows 官方状态为 alpha：专用本地账户 `srt-sandbox` + 按 SID 的机器级 WFP 围栏 + NTFS ACE + 写受限 token，需一次管理员 `windows-install`。PR #427（2026-10-21 仍 open）增加第二条路径：`initialize()` 时用真实策略探测，只有干净的 `base-container` 结果才选 MXC（经 `wxc-exec.exe`），否则继续 srt-win；"MXC 的 AppContainer 层从不使用，其网络强制是合作式的，无管理员时防火墙规则写入失败会无围栏继续" |
| Claude Code | issue #69495 请求采用 MXC，尚无官方集成 |
| NVIDIA OpenShell | 在 MXC 之上集成 |

## 5. 与其他 Windows 沙箱方案的比较

| 维度 | MXC BaseContainer | SRT srt-win（alpha） | 自研（Codex windows-sandbox-rs 式） |
| --- | --- | --- | --- |
| 安装/提权 | 无安装，每次启动无 UAC | 一次管理员安装（建账户、WFP 过滤器、DPAPI 口令）；升级需重跑 | 需要服务或一次性设置 |
| OS 要求 | 2026-08 之后的 Win11 24H2/25H2 | 任意 Win11 | 任意 Win11 |
| 文件边界 | OS 持有的策略，三列表 | 写 ACE 到用户文件树（首次按树大小耗时分钟级，跨会话保留写授权；任何沙箱会话可读已授权根） | 受限 token + ACL |
| 网络边界 | 按容器 SID 的 WFP，失败关闭 | 按账户 SID 的 WFP，线程换 token 可逃逸（文档已列） | 自行实现 |
| TLS | 未知（SRT PR 列为待验证项） | 受限 token 下 Schannel 不可用：System32 curl、PowerShell 网络 cmdlet、.NET HttpClient、winget、cargo 失效 | 同左类问题 |
| 维护方 | 微软，OS 原生 | Anthropic，社区节奏 | 自己 |

## 6. 对 DAgent 的意义

DAgent 现有的执行模型与 MXC 的契约高度对应：

- `exec::Policy` 的 `readable` / `writable` / `protected_read` / `protected_write` 分别映射到 `readonlyPaths` / `readwritePaths` / `deniedPaths`；
  `private_tmp` 对应容器自己的 `%TEMP%`/`LOCALAPPDATA` 虚拟化。
- 运行中网络审批闸门（目前经 SRT 代理实现 host:port 级放行）对应 MXC 的代理模式：DAgent 自己的回环代理做目的地过滤，MXC 保证容器没有直连能力。
- `sandbox status` 的"静态清单 + 一次真实启动探测"与 MXC 的 `--probe` / `mxc_probe_request_json` 探针天然契合。

接入方式有三种：

1. 等 SRT PR #427 合并后沿用现有 `srt_bridge.mjs`：改动最少，但要接受 SRT Windows alpha 的安装步骤、ACL 写入与 Schannel 限制，
   且 MXC 的选择权在 SRT 手里。
2. 直接从 C++ 加载 `mxc_ffi.dll`（C ABI，JSON 进出，`mxc_spawn_json` 提供流式 stdio 与句柄）：最贴合 DAgent（无 Node 依赖、
   进程树与取消由我们已有的 Job Object 代码接管）。需要先确认 DLL 的分发渠道（Release zip 或 NuGet 包的 `runtimes/win-x64/native`）与体积。
3. 以 `wxc-exec.exe` 子进程 + JSON 调用：最简单，多一层进程与 stdio 转发，适合做探针或回退。

建议：以方案 2 为主、方案 3 做探针；**只使用 BaseContainer 层**（与 Codex、SRT 的判断一致），探测结果不是 `base-container`
时继续按当前"无沙箱"模式运行，并让 `sandbox status` 报告所缺的 OS 版本，而不是回退到 AppContainer 的合作式网络限制。
`fallback.allowDaclMutation=false` 可在请求层面拒绝 DACL 回退。

前置条件：开发虚拟机目前是 26200.8037（2026-03 更新，`System32` 下没有 `processmodel.dll`），需要升级到 26200.9278 以上才能验证。

工作量粗估（不含 Windows 上的 stdio MCP 与 web 工具，它们依赖只读 SRT 语义，需另行映射）：探测与状态报告、策略映射、
执行（流式输出、超时、取消、Job）、网络代理接线、文档，约 2–3 人周，与早先对第三步的估计一致。

## 7. 风险与未知

- SDK 1.0 刚发布，0.x → 1.0 全量重命名，文档建议锁定版本；原生库未作为独立产物发布，需要验证提取方式。
- `deniedPaths` 语义按后端而异；BaseContainer 无只写拒绝，受保护路径可能变成不可读。
- 容器内 Schannel 可用性、宿主回环代理可达性、`ingress.hostLoopback` 对无身份代理的要求（会同时打开双向回环）均需在真实机器上验证。
- 前台进程退出时 MXC 终止全部后代（与 DAgent 当前"后台进程随主进程清理"的语义一致，但与 Codex 旧后端不同）。
- 命令行长度受 Windows 限制；路径与环境值必须是合法 Unicode。
- 遥测同意提示由 MXC 自带，需要在首次使用时处理用户体验。

## 8. 开发虚拟机上的探测记录（2026-10-22）

目的：确认 MXC 的 BaseContainer 层在一台打满补丁的 Windows 11 25H2 上是否可用。

| 步骤 | 结果 |
| --- | --- |
| 升级前 | Windows 11 Pro 25H2，build 26200.8037（2026-03 累积更新），`System32` 中没有 `processmodel.dll` |
| 离线安装 KB5127648（2026-10 累积更新，含检查点包 KB5043080；Microsoft Update Catalog 下载，`DISM /Online /Add-Package`） | 约 50 分钟，退出码 3010，重启后 build 26200.9711 |
| `wxc-exec.exe --probe`（MXC SDK 1.0.1 的 x64 执行器） | `tier = appcontainer-dacl`，`baseContainerApiPresent = false`，`isolationSessionAvailable = false`，`hyperlightAvailable = true`；告警提示需要 `wxc-host-prep prepare-system-drive` / `prepare-null-device`（均需管理员） |
| `System32` 检查 | 仍没有 `processmodel.dll`；没有 `api-ms-win-*securityenvironment*` API set 文件 |
| `WinSxS` 检查 | 存在组件 `amd64_microsoft-windows-a..nmodel-processmodel_…_10.0.26100.9711`，其中有 `processmodel.dll` 10.0.26100.9711，但未投影到 `System32` |

结论：

- 在 25H2 上，PSEC（`CreateProcessSecurityEnvironment` 等导出所在的 `processmodel.dll`）随累积更新一起下发到了组件仓库，但处于**未启用**状态。
  这与 MXC 文档"符号存在但特性被禁用"的措辞一致，也与公开报告一致：一台 26H2（26300.9711）机器的 `System32` 里有同版本号
  10.0.26100.9711 的 `processmodel.dll`，而 26H2 与 25H2 共用同一服务分支，仅由启用包（KB5121794，约 180 KB）切换特性。
  因此 MXC 文档中"25H2 ≥ 26200.9278 支持进程隔离"的表述，至少在这台机器上不成立；实际门槛看起来是 26H2 启用包。
- 启用包 KB5121794 不在 Microsoft Update Catalog 中（仅通过 Windows Update 分发），而本机环境到 Windows Update 端点不通；
  现成的离线来源只有第三方镜像。本次没有继续安装。
- 即便 API 存在，公开 issue（openai/codex #53650）显示普通用户会话中 `CreateProcessSecurityEnvironment` 在 26300.9711 上
  返回 `0x80070005`（E_ACCESSDENIED），原因未明，微软尚无文档说明权限要求。这意味着 BaseContainer 目前对最终用户并不稳定可用。
- 不选 BaseContainer 时，MXC 回退到 AppContainer + DACL：需要管理员一次性修改系统盘根 ACL，每次开机后修补 `\Device\Null`，
  网络只有能力粒度而没有 WFP 规则。这一层不满足 DAgent 对"失败关闭"网络边界的要求，Codex 与 SRT 也都拒绝使用它。

对 DAgent 的影响：第三步（Windows 沙箱）暂不启动开发。重新评估的触发条件：(1) 26H2 启用包可以通过可信渠道离线获得，
或虚拟机能访问 Windows Update；(2) `CreateProcessSecurityEnvironment` 的 E_ACCESSDENIED 有官方解释或在后续更新中消失；
(3) SRT PR #427 合并并在真实机器上验证通过。届时先在虚拟机上用 `wxc-exec --probe` 确认 `tier = base-container`，再决定接入方式。

探测材料：`/home/jyt/winvm/shared/mxc/bin/`（SDK 1.0.1 的 x64 二进制）、`/home/jyt/winvm/shared/dagent/jobs/vmupdate/`（安装与探测脚本及输出）。
升级前的整盘备份在 `/home/jyt/winvm/storage-pre-update-20261022/`。

## 来源

- openai/codex issue #53650（CreateProcessSecurityEnvironment 0x80070005）：https://github.com/openai/codex/issues/53650
- Windows 11 26H2 启用包 KB5121794 的第三方报道（未在 Update Catalog 中）：https://pureinfotech.com/windows-11-26h2-enablement-package-download/
- Windows Developer Blog，2026-10-07：https://blogs.windows.com/windowsdeveloper/2026/10/07/microsoft-execution-containers-policy-driven-containment-for-ai-agents/
- Windows Experience Blog，2026-10-07：https://blogs.windows.com/windowsexperience/2026/10/07/building-windows-for-hybrid-intelligence/
- 仓库与文档：https://github.com/microsoft/mxc（`README.md`、`docs/containment-policy/1.0.0/policy.md`、`docs/backends/process-container/os-version-support.md`、`docs/backends/process-container/networking.md`、`docs/backends/process-container/host-prep.md`、`src/ffi/mxc_ffi/src/lib.rs`、`sdk/dotnet/README.md`）
- npm：https://www.npmjs.com/package/@microsoft/mxc-sdk ；crates.io：https://crates.io/crates/mxc-sdk
- 第三方开发者指南：https://www.developersdigest.tech/blog/microsoft-mxc-developer-guide-2026
- VentureBeat：https://venturebeat.com/security/microsoft-launches-mxc-an-os-level-sandbox-for-ai-agents-with-openai-and-nvidia-already-on-board
- InfoWorld：https://www.infoworld.com/article/4215416/running-ai-agents-in-sandboxes-with-microsoft-execution-containers.html
- GitHub Copilot CLI 沙箱：https://commandline.microsoft.com/local-models-sandboxed-tools-github-windows/
- OpenAI Codex：https://github.com/openai/codex/tree/main/codex-rs/mxc-sandbox ；https://openai.com/index/building-codex-windows-sandbox/
- Anthropic sandbox-runtime：https://github.com/anthropics/sandbox-runtime（README "Windows (alpha)"）；PR #427 https://github.com/anthropics/sandbox-runtime/pull/427 ；Claude Code issue https://github.com/anthropics/claude-code/issues/69495
- NVIDIA：https://developer.nvidia.com/blog/build-personal-ai-agents-on-windows-pcs-with-new-tools-from-microsoft-and-nvidia/
