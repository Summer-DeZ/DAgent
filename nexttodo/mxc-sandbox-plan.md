# 沙箱后端切换到 MXC

批准：2026-10-22。调研依据见 [MXC 调研](../docs/research/2026-10-22-microsoft-mxc.md)；当前 SRT 实现见 [exec](../docs/design/exec.md) 和 [agent §7](../docs/design/agent.md#7-权限与沙箱)。

## 目标

受限执行从 Anthropic Sandbox Runtime（SRT，Node bridge + bubblewrap + socat 代理）切换到 Microsoft Execution Containers（MXC）1.0.1 的原生 C ABI 库，Linux 与 Windows 共用一套执行代码。完成后移除 SRT：`srt.hpp`、`srt.cpp`、`srt_bridge.mjs`、`srt_bwrap.sh`、`internal/sandbox` Node 环境、`home/config/sandbox/`、socat 依赖和 `SrtControl`。

权限模型不变：`ask / workspace / unrestricted` 与正交的 `read_only / planning`、运行中网络审批、host access、MCP profile、子 Agent 收窄与父审批均保持现有语义，只替换执行边界的落实方式。

## 已验证事实（2026-10-22，本机 Ubuntu 22.04，bwrap 0.6.1，glibc 2.35）

- `libmxc_ffi.so` 1.0.1 可直接 dlopen；`mxc_platform_support_json` 报告 bubblewrap 可用、`proxyEnforcement: supported`（需 slirp4netns）。
- FFI 真实执行受限命令成功：以当前用户身份运行、`/tmp` 为私有 tmpfs、未列出的路径不可见、`deniedPaths` 读取被拒；超时返回 `timed_out=1`；信号终止以 128+N 退出码报告。
- 代理模式的宿主前置（slirp4netns、unshare、nsenter、ip、nf_tables 后端 iptables、nf_conntrack）本机齐备。
- Windows 开发虚拟机（25H2 build 26200.9711）探测层级为 `appcontainer-dacl`，BaseContainer 不可用；本期 Windows 无法验收真实隔离执行。

## 用户决定

1. Windows 只接受 ProcessContainer 的 `base-container` 层；探测为其它层时按"无沙箱"运行，`sandbox status` 报告层级与所缺条件。不使用 AppContainer + DACL 回退层，请求中设置 `fallback.allowDaclMutation=false`。
2. 原生库经 `runtime.json` 登记为 `mxc` 包，`runtime sync` 从 npm 下载 `@microsoft/mxc-sdk@1.0.1` 的 tgz 并按 SHA-256 校验，运行时加载其中 `bin/<arch>/libmxc_ffi.so` 或 `mxc_ffi.dll`。二进制不进仓库。
3. Linux 宿主依赖改为：bwrap ≥ 0.5、slirp4netns、util-linux（unshare/nsenter）、iproute2、iptables（nf_tables 后端）、nf_conntrack 模块、glibc ≥ 2.34；去掉 Node 的 `internal/sandbox` 环境与 socat。
4. `sandbox.version` 升到 3，`sandbox.backend` 唯一合法值为 `mxc`；旧配置报错并提示手改，不做静默迁移。

## 技术决策

- 库在运行时 dlopen / LoadLibrary，不在链接期依赖。FFI 原型手写在 `src/public/exec/mxc.hpp`（MXC 不发布 C 头文件，其 C ABI 随库版本走），探测时校验 `mxc_version() == "1.0.1"`，不匹配即后端不可用。
- 执行用 `mxc_spawn_json` 加阻塞式 `mxc_stream_read` 读线程（stdout、stderr 各一），退出用 `mxc_sandbox_try_wait` 轮询，取消与超时在同一线程调用 `mxc_sandbox_kill`；两平台共用。
- 策略映射（`Policy` → 1.0.0 策略 JSON）：`readable` → `readonlyPaths`；`writable` 与私有 HOME → `readwritePaths`；`protected_read` 及可读树内的敏感路径 → `deniedPaths`；`protected_write` 及可写树内的敏感路径 → 嵌套在可写路径内的 `readonlyPaths`；`read_exceptions` 照旧剔除。只读搜索与上下文命令省略 `network`，不启动 slirp4netns。
- 网络：DAgent 自行实现进程内回环 HTTP 代理（`exec/proxy.hpp`、`exec/proxy.cpp`，Asio，CONNECT 与明文 HTTP 转发），通过 `runtimeConfig.networkProxy` 交给 MXC；每次执行独占一个 127.0.0.1 随机端口，目的地按 host:port 调用现有 `NetworkGate`，保留审批超时、每执行请求预算、拒绝即取消的语义。MCP stdio 的严格 allowlist 由同一代理按 profile 判定，无运行中审批。若 MXC 允许代理 URL 携带凭据，附加每次执行的随机口令。
- stderr 并入 stdout 通过命令行层面的 `2>&1` 实现；命令行形如 `cd <workspace> && exec <managed bash> -c <quoted command> 2>&1`，Windows 用对应的命令行拼接。
- 边界标注字符串由 `srt` 改为 `mxc`（Grant.backend、Support.backend、MCP boundary、协议与界面）。
- 环境与私有目录沿用现状：HOME、XDG、TMPDIR、uv/npm 缓存指向 `/tmp/dagent-home-*`；不再需要 bridge 控制目录。
- AppArmor 模板按 S0 结论调整（bwrap、unshare、slirp4netns 现在由 `dagent-backend` 直接生成）。

## 阶段

分支 `platform/mxc-sandbox`，一步一个提交。实现类子 Agent 使用 Opus。

### S0 可行性验证

只在 `temp/mxc-check/` 写真实检测材料，不改项目代码。用 FFI 确认：嵌套在 `readwritePaths` 内的 `readonlyPaths` 写入得到 EROFS；`/tmp/dagent-home-*` 绑定进 tmpfs `/tmp` 后可写；代理模式下容器内 curl 经 `HTTPS_PROXY` 可达宿主代理、直连被拒、代理 URL 能否带凭据；现有 AppArmor profile 是否足够。任一项失败先修订本计划。

### S1 exec 模块（Linux）

`exec/mxc.hpp`、`exec/mxc.cpp` 替换 SRT 文件：库加载与版本校验、策略编译、spawn / 流 / 等待 / 取消、`Support` 真实探测。新增 `exec/proxy.hpp`、`exec/proxy.cpp`。`ReadOnlySandbox` 改为无网络策略。`platform.hpp` 删除 `SrtControl`，增加平台命令行拼接。验收：`temp/platform-check/runs.py` 的 bash、grep、edit、网络审批、取消、超时场景；`det.sh` 比对；ask 模式审批截帧。

### S2 装配、工具与 MCP

`tools/sandbox.cpp`、`tools/bash.cpp`、`tools/web.cpp`、`web/http.cpp`、`mcp/stdio_transport.cpp`、`app/bootstrap.cpp`、`app/sandbox_status*.cpp`、`app/config.cpp`、`backend/convert.cpp` 改用新类型；MCP stdio 用 `mxc_spawn_json` 的原生管道直接接 `StdioTransport`。`runtime.json` 登记 `mxc` 包并删除 `internal/sandbox`、`home/config/sandbox/`；CMake 删除 bridge 复制与安装；AppArmor 模板调整。验收：web_search / web_fetch、`temp/refactor-verify/mcp` 的真实 stdio 服务器、子 Agent 降权取消、`dagent sandbox status` 完整报告。

### S3 Windows 接入

`mxc_ffi.dll` 加载、`mxc_probe_request_json` 探测并只接受 `base-container`、`fallback.allowDaclMutation=false`、Windows 命令行拼接、`sandbox status` 报告层级与所缺 OS 版本。验收：虚拟机 Release 构建零警告、`sandbox status` 报告 `appcontainer-dacl` 并以无沙箱模式启动、TUI 欢迎页提示原因。与 S1、S2 并行。

### S4 文档与收尾

同步 `docs/design/exec.md`、`agent.md §7`、`platform.md`、`web.md`、`mcp.md`、`toolchain.md`、`home.md`、`guide/build.md`、`guide/permissions.md`、`guide/workflow.md`；按 [workflow §2.3](../docs/guide/workflow.md) 做整套回归；`--no-ff` 合并进 master；调研文档第 8 节补本次结论；本计划归档。

## 本期不做

- Windows BaseContainer 的真实隔离验收（等 26H2 可离线获得或 `E_ACCESSDENIED` 有解释）。
- macOS seatbelt 后端、hyperlight / microvm / lxc 等其它后端。
- MXC 的 Learning / Permissive 模式与活动报告。
