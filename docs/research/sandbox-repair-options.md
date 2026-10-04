# 沙盒修复方案比较

日期：2026-09-29。状态：实施前方案比较，以下观测与源码行号对应当时版本。

用户随后选定 SRT；2026-09-30 的实现已移除 Landlock 后端，专用 AppArmor profile 已使开发主机普通启动通过。
本页保留当时的方案比较，未采用的备选不代表仍受支持。当前行为见 [执行设计](../design/exec.md)，
部署见 [构建指南](../guide/build.md)，原路线见 [SRT 计划归档](../archive/2026-09-30-srt-permissions-plan.md)，剩余验收见 [后续工作](../../nexttodo/README.md)。

## 已核实的起点

- 本轮会话实测：bubblewrap 0.9.0 和 `unshare -Ur` 在 UID 映射阶段失败；AppArmor 非特权 user namespace 限制开启。尚无内核审计日志，不能认定 AppArmor 是唯一原因。
- DAgent 真实 `pwd` 调用成功加载 `landlock-seccomp-v2 / read_only`；`curl` 调用在非交互模式因需要一次性宿主权限而未执行。记录在 `temp/sandbox-diagnosis-20260929/`。这不是完整网络隔离验收。
- 当前 [能力探测](../../src/private/exec/sandbox.cpp#L356) 固定报告 `protected_subpaths=false`；[授权策略](../../src/private/agent/permission.cpp#L292) 在 workspace 沙盒缺失时请求 host access；[宿主授权](../../src/private/agent/permission.cpp#L135) 同时打开网络。
- 上述限制属于 Bash 执行路径，不能推导为模型 API、所有 MCP 服务或整个 DAgent 已受同一沙盒保护。

## 第一方资料

1. [Ubuntu 24.04 发布说明：非特权 user namespace 限制](https://documentation.ubuntu.com/release-notes/24.04/)：系统提供针对应用的 AppArmor profile 许可路径。具体 profile 仍须结合本机审计结果确认。
2. [Anthropic sandbox-runtime](https://github.com/anthropics/sandbox-runtime#how-it-works)：Linux 使用 bubblewrap 隔离，网络经宿主代理按域名控制；项目仍标为研究预览。
3. [Linux 7.0 Landlock 文档](https://docs.kernel.org/7.0/userspace-api/landlock.html)：文件权限规则按层级授权；TCP 网络规则以端口为对象。不能把端口许可解释成域名许可。
4. [已有固定版本后端调研](command-sandbox-backend.md)：包含 sandbox-runtime v0.0.77 的 user namespace 依赖、Unix socket、文件路径和生命周期限制。该版本号是历史研究对象，不代表当前应直接安装的版本。

## 方案取舍（工程判断）

| 方案 | 收益 | 成本与适用范围 |
| --- | --- | --- |
| 专用 AppArmor profile + sandbox-runtime | 复用文件隔离和受控网络出口，减少 DAgent 自研范围 | 需管理员完成主机前置；固定版本、适配进程生命周期并真实验收 |
| 直接集成 bubblewrap + 自研代理 | C++ 侧控制直接 | 同样受主机限制；代理、地址策略、路径覆盖和清理均需自行维护 |
| 保留 Landlock 只读 Shell + 受控文件工具 | 可沿用当前可运行路径，不依赖新增 user namespace 权限 | 不能覆盖任意构建、安装、脚本写入；文件工具仍须遵守独立路径授权 |
| 独立容器或虚拟机执行端 | 主机无法提供所需能力时，可迁移执行边界 | 新增运行环境、工作区同步与进程管理成本；容器是否可用须另外验证 |

推荐优先评估第一项。它不绕过系统限制，而是把一次性部署前置与每次工具调用的授权分开。
不以全局关闭 AppArmor 限制作为默认部署方式，不把研究预览后端宣称为已验证的完整保障。

## 建议的职责和授权边界

- DAgent 的权限层负责用户决策；执行层负责把决策落实到 OS 边界。不要让授权策略拼接后端命令行。
- 执行授权分别表达文件读取、文件写入、网络目标和宿主执行；已有 `ExecutionGrant` 可作为收敛入口，避免增加第二套授权状态。
- 用户批准某个网络目标后，仅给当前受限执行增加该目标，文件范围保持原授权；不能继续映射为 `full_access`。
- 网络由沙盒不可绕过的代理出口控制；仅设置 HTTP_PROXY 或允许 TCP 443 不构成域名隔离。域名许可也不代表远端操作只读。
- 控制进程负责模型通信与凭据；隔离工具子进程及其子孙。对本地 MCP 的隔离另外接入，远端 MCP 保留独立调用授权，不把 Bash 后端覆盖范围夸大到整个应用。
- 执行层拥有子进程、代理端点和临时资源的生命周期；退出、取消与启动失败均应清理。授权记录应反映真正采用的后端和边界。
- 后端能力不足时明确报告缺失条件。保留用户显式选择的单次宿主执行，但不能将普通网络授权扩大为宿主执行授权。

## 实施顺序与真实验收

1. 获取本机拒绝日志，确认管理员可部署专用 profile；用真实 bubblewrap 命令验证 user/mount/PID/network namespace 的完整组合。仅 UID 映射成功不够。
2. 选定并固定后端版本，核对接口和依赖，将其纳入项目已有 runtime 管理；主机 AppArmor 配置作为单独部署前置。
3. 在执行层增加一个明确的后端适配，落实独立文件和网络授权，保留调用取消、退出码与资源回收语义。
4. 在 `temp/` 下以真实工具运行确认：允许区写入、受保护区拒绝、无授权网络拒绝、批准域名可访问、直接 IP/其他目标/本地 socket 不能越权、子进程继承以及取消清理。只做构建与真实运行，不创建测试代码或模拟服务。

如第一步主机条件无法满足，先采用能力受限的只读 Shell 路径；需要完整自动构建时再考虑独立执行环境。更换一个仍依赖 user namespace 的库不能消除该前置。

## 开源项目参照

以下是 2026-09-29 查询到的上游主分支文档，不代表所有已发布版本或用户默认配置。

| 项目 | 官方文档描述的实现 |
| --- | --- |
| [Codex CLI](https://github.com/openai/codex/blob/main/codex-rs/linux-sandbox/README.md) | Linux 默认文件沙盒采用 bubblewrap：只读根、可写工作目录、嵌套保护路径重新只读挂载；受控网络模式使用独立 network namespace 和代理桥接。user namespace 不可用会提示；需要 bubblewrap 的不支持环境会拒绝相应执行。 |
| [Anthropic sandbox-runtime](https://github.com/anthropics/sandbox-runtime#how-it-works) | Linux 使用 bubblewrap 和宿主 HTTP/SOCKS 代理，分别实现文件与域名网络限制；可作为独立库或 CLI 集成。 |
| [Gemini CLI](https://github.com/google-gemini/gemini-cli/blob/main/docs/cli/sandbox.md) | 支持 Docker/Podman 等隔离方式，将工作区挂入执行环境；也有平台相关后端。启用文件隔离不能直接解释为网络默认被封锁。 |
| [OpenHands Docker Runtime](https://github.com/OpenHands/docs/blob/main/openhands/usage/architecture/runtime.mdx) | Agent 通过 API 调用 Docker 容器内的执行服务，Shell、浏览器等工具在该执行环境运行。 |
| [OpenCode](https://github.com/anomalyco/opencode/blob/dev/SECURITY.md#no-sandbox) | 明确声明权限提示不提供安全隔离；需要真正隔离时由用户部署 Docker 或虚拟机。 |

归纳：现有项目有应用审批、进程沙盒和独立执行环境几类选择，并非所有项目都承诺 OS 级隔离。对采用严格网络限制的进程沙盒，域名代理必须与直接网络隔离配合。不能把代理环境变量或容器这一名称直接当作网络白名单的证明。
