# 命令沙箱后端调研：sandbox-runtime、bubblewrap 与 Landlock

日期：2026-09-20  
范围：当时权限计划的 P01 有界调研；原计划路径已退出当前文档结构。这里只保留 Linux 后端选型依据。
来源约束：仅使用 Anthropic、bubblewrap 项目、Linux 内核及 Ubuntu 的官方/第一方资料。  

历史说明（2026-09-30）：下文“当前”“尚未安装”“不可运行”均指调研时点。
项目后来固定采用 SRT 0.0.77，移除旧 Landlock 后端，并部署专用 AppArmor profile 通过默认启动。
本页不再作为部署步骤或现有能力判断；请参阅 [执行设计](../design/exec.md)、
[构建指南](../guide/build.md) 和 [验收摘要](../archive/2026-09-30-srt-permissions.md)。

## 结论

截至本次调研，Anthropic Sandbox Runtime 最新已发布版本是
[`v0.0.77`](https://github.com/anthropics/sandbox-runtime/releases/tag/v0.0.77)，仍标为
Beta Research Preview；首版若采用它应固定该版本，不能跟随 `latest`。

**本机当前不能把 sandbox-runtime 作为可运行后端。** 它在 Linux 上不是 bubblewrap 的替代品，
而是始终用 bubblewrap 建立 mount/user/PID namespace，并在需要网络隔离时建立 network namespace；
默认的 Unix socket 过滤还会再建立一层 user/PID/mount namespace。官方源码即使启用
`enableWeakerNestedSandbox` 也仍加入 `--unshare-user`，该选项只改变 `/proc` 的处理方式，并不绕过
user namespace。参见
[`linux-sandbox-utils.ts` 的 namespace 组装](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/src/sandbox/linux-sandbox-utils.ts#L3147-L3153)和
[`--unshare-user`/弱嵌套分支](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/src/sandbox/linux-sandbox-utils.ts#L3250-L3293)。

本机 `bwrap 0.9.0` 的最小 user namespace 探针已返回
`bwrap: setting up uid map: Permission denied`，完整网络 namespace 组合另返回
`bwrap: loopback: Failed RTM_NEWADDR: Operation not permitted`。所以 sandbox-runtime 在不改变宿主
AppArmor/sysctl/运行身份或部署边界的前提下**不能绕过该限制**。Anthropic 的安装说明也明确要求
Linux 上存在“保有 capability 的 user namespace”，并把关闭相应 sysctl 或增加允许 `userns` 的
AppArmor profile 列为宿主侧处理方式，而不是 runtime 内部降级路径；参见
[v0.0.77 Linux 依赖说明](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#platform-specific-dependencies)。

Landlock 可继续作为不依赖 user namespace 的保守文件系统限制组件，但本机 ABI 7 不能单独满足
计划中的完整边界：它无法在一条“workspace 父目录可写”规则下再用子目录规则撤销写权限；网络只按
TCP 端口控制 bind/connect，不能表达域名或目的地址；ABI 7 只能隔离跨 Landlock domain 的 abstract
Unix socket 连接，不能按路径授权 pathname Unix socket，也不能收回继承或预先打开的 socket fd。
因此不能把现有 Landlock + seccomp 路径标成与 sandbox-runtime 等价的完整 workspace 后端。

## 已核实事实

### 1. 本机观测

以下均为 2026-09-20 的只读诊断或实际隔离启动，没有修改 sysctl、AppArmor 或系统包：

| 项目 | 结果 |
| --- | --- |
| 内核/架构 | `Linux 6.17.0-1021-nvidia aarch64` |
| bubblewrap | `/usr/bin/bwrap`，`bubblewrap 0.9.0` |
| 其他依赖 | `/usr/bin/socat` 1.8.0.0、`/usr/bin/rg` 14.1.0 |
| Node/npm | Node 24.17.0、npm 11.13.0，满足包声明的 Node `>=20.11.0` |
| sandbox-runtime | `srt` 未安装 |
| AppArmor | 已启用；`kernel.apparmor_restrict_unprivileged_userns=1` |
| 通用 userns 开关 | `kernel.unprivileged_userns_clone=1` |
| Landlock | `landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)` 返回 ABI 7 |

实际命令与结果：

```text
$ bwrap --unshare-user --dev-bind / / true
bwrap: setting up uid map: Permission denied
exit 1

$ bwrap --ro-bind / / --unshare-net --unshare-pid --proc /proc --dev /dev -- true
bwrap: loopback: Failed RTM_NEWADDR: Operation not permitted
exit 1
```

`setting up uid map` 是 bubblewrap 写入 user namespace 的 `uid_map` 失败时直接产生的错误；可对照
[bubblewrap 0.9.0 源码](https://github.com/containers/bubblewrap/blob/v0.9.0/bubblewrap.c#L982-L1005)。
本机同时满足“通用 userns 开启、AppArmor 的 unprivileged userns 限制开启”，且 Ubuntu 官方说明
24.04 起该限制默认启用，并会拒绝未获 profile 许可的 capability 使用；参见
[Ubuntu 24.04 发布说明](https://discourse.ubuntu.com/t/ubuntu-24-04-lts-noble-numbat-release-notes/39890#p-1928937-unprivileged-user-namespace-restrictions-14)和
[Ubuntu AppArmor `userns` 规则说明](https://manpages.ubuntu.com/manpages/noble/man5/apparmor.d.5.html#user%20namespace%20rules)。
这使 AppArmor 成为与两个实测错误一致的首要原因，但本次没有取得对应 kernel audit denial，故只记为
**高可信定位，不写成已由审计日志证明的唯一原因**。

### 2. sandbox-runtime 版本、安装、依赖与许可证

`v0.0.77` 于 2026-09-18 发布；其
[`package.json`](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/package.json#L1-L13)
声明包名 `@anthropic-ai/sandbox-runtime`、Node `>=20.11.0` 和 CLI `srt`。官方记录的安装命令是
`npm install -g @anthropic-ai/sandbox-runtime`，项目仍明确标注研究预览，API 和配置格式可能变化；参见
[固定版本 README](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#anthropic-sandbox-runtime-srt)。

Linux 运行依赖是 `bubblewrap`、`socat` 和 `ripgrep`；x86-64 与 arm64 的静态
`apply-seccomp` 和 BPF filter 已随 npm 包分发，正常安装不需要现场编译。其他架构没有该 Unix socket
过滤支持；从源码重建过滤器才需要 C 编译器和 `libseccomp-dev`。参见
[平台依赖](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#platform-specific-dependencies)和
[seccomp 构建说明](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#building-seccomp-binaries)。

项目许可证为 Apache License 2.0，见
[`package.json` 的 SPDX 标识](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/package.json#L72-L78)和
[完整 LICENSE](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/LICENSE)。

### 3. CLI 与策略能力

CLI 可用 `srt <argv...>` 包装命令，支持 `--debug`、`--settings <path>`、`-c <command>` 和
`--control-fd <fd>`；完整定义见
[`src/cli.ts`](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/src/cli.ts#L256-L274)。
显式指定但不存在、不可读、为空或校验失败的 settings 文件会拒绝执行，不会静默退回默认配置；没有
settings 文件时的内建默认值是禁网、默认路径之外不可写、读取不受限。参见
[CLI settings 契约](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#as-a-cli-tool)。

`--control-fd` 能在命令运行期间替换配置，但**只有** `allowedDomains`/`deniedDomains` 会立即生效；
文件系统规则在 wrap 时已经编译，运行中更新对当前进程无效。这意味着 DAgent 若走 CLI，目录授权必须在
每次启动前生成完整 settings，不能把运行中的目录审批接到 `--control-fd`。参见
[`--control-fd` 契约](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#updating-the-config-while-the-command-runs---control-fd)。

文件系统策略支持：

- 读取采用 `denyRead` 后由 `allowRead` 重开；默认读取全部允许。
- 写入采用 `allowWrite` 白名单，并由 `denyWrite` 在已允许父路径内排除子路径；默认全部不可写。
- Linux 后端通过具体 bind mount 实现，write 路径不支持一般 glob；read glob 只对 wrap 时已存在的路径展开。
- Linux 可表达“workspace 可写、其中控制目录不可写”，但它依赖 mount 覆盖、路径钉住及启动时扫描，
  不是 Landlock 的负向子路径规则。

依据见
[文件系统配置](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#filesystem-configuration)、
[Linux 路径语义](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#path-syntax-linux)和
[读写优先级及 Linux 实现说明](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#filesystem-isolation)。

网络策略默认全拒绝，允许项按域名/通配域名及可选端口表达；Linux 先用 `--unshare-net` 移除直接网络，
再通过 bind-mounted Unix socket、`socat` 和宿主 HTTP/SOCKS5 proxy 提供受控出口。域名过滤发生在
proxy，而不是 Landlock 或 bubblewrap 内核策略中。参见
[网络配置](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#network-configuration)和
[Linux proxy 架构](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#network-isolation-architecture)。

Linux 的 Unix socket 控制不是路径白名单：`allowUnixSockets` 在 Linux 上被忽略；默认 seccomp 过滤
直接拒绝新建 `AF_UNIX` socket，`allowAllUnixSockets: true` 会整体关闭该过滤。过滤不阻止继承的
Unix socket fd，也不阻止通过 `SCM_RIGHTS` 得到的 fd。参见
[Unix Socket Settings](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#network-configuration)和
[过滤器限制](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#unix-socket-restrictions-linux)。

CLI 收到 `SIGINT`/`SIGTERM` 时会转发给直接 child；child 因这两个信号结束时，CLI 自己退出 0。
控制通道在首次更新前失败时，CLI 先发 `SIGTERM`，两秒后发 `SIGKILL`。这些是直接 child 的行为；
DAgent 仍需自行保留进程组/调用状态语义，不能用 `srt` 的退出码 0 推断正常完成。参见
[`src/cli.ts` 的 spawn、退出与信号处理](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/src/cli.ts#L499-L553)。

本机因 namespace 准备阶段失败，无法形成有效的启动时延数据；本报告不推测或填写性能数字。

### 4. 为什么 sandbox-runtime 不能绕过本机 uid_map EPERM

1. sandbox-runtime 官方声明 Linux 后端就是 bubblewrap，并要求 capability-bearing user namespace；
   参见[平台说明](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#platform-support)。
2. Linux 网络隔离始终加入 `--unshare-net`；文件/PID 隔离路径始终加入 `--unshare-pid` 和
   `--unshare-user`。弱嵌套模式仍保留 `--unshare-user`，只把 fresh `/proc` 改为绑定宿主 `/proc`；
   参见[实现](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/src/sandbox/linux-sandbox-utils.ts#L3147-L3153)及
   [namespace 尾部](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/src/sandbox/linux-sandbox-utils.ts#L3250-L3293)。
3. 默认 Unix socket 过滤器还会创建内层 user/PID/mount namespace；若它失败则中止，不会无提示运行；
   参见[两阶段 seccomp 设计](https://github.com/anthropics/sandbox-runtime/blob/v0.0.77/README.md#unix-socket-restrictions-linux)。
4. 即使设置 `allowAllUnixSockets: true` 跳过内层 seccomp helper，外层 bubblewrap 的
   `--unshare-user` 仍存在，所以不能解决当前 `uid_map` EPERM。
5. bubblewrap 本身是构建 namespace 的低层工具，安全边界取决于调用参数，不提供另一套无 userns 的
   等价策略；参见[bubblewrap 0.9.0 安全模型](https://github.com/containers/bubblewrap/blob/v0.9.0/README.md#sandbox-security)。

因此可行条件只能来自 sandbox-runtime 外部，例如管理员为相关可执行文件提供允许 `userns` 的
AppArmor profile、调整宿主限制，或把执行器部署到允许这些 namespace 的环境。它们都属于宿主部署
决策，本次没有实施。

### 5. Landlock 能力边界（本机 ABI 7）

#### 5.1 受保护子目录

Landlock 是 allow-list 模型。对一个 policy layer 而言，只要路径上遇到的任一规则授予某项访问，
该层就授予它；多个 layer 之间再取交集。内核文档明确说明该组合语义及限制只可继续叠加、不可移除，见
[ruleset layer 语义](https://docs.kernel.org/6.17/userspace-api/landlock.html#layers-of-file-path-access-rights)。

由此可直接推出：若同一层对 workspace 父目录授予完整写权限，该授权覆盖其所有后代；再给受保护
子目录添加“较少权限”的规则不会撤销父规则，因为 Landlock 没有 deny rule 或“最具体规则优先”。
额外 layer 也只有在该层能以叶子白名单描述 workspace 其余部分时才有用；给 `/` 或 workspace 父目录
补一条广泛 allow 又会重新覆盖受保护子目录。官方因此建议尽量在 hierarchy leaves 上授予权限，而非
先授予大父目录；见
[Landlock good practices](https://docs.kernel.org/6.17/userspace-api/landlock.html#good-practices)。

可行方案是只允许明确叶子/兄弟层级，或再结合 mount namespace/其他 LSM 把受保护路径遮蔽；动态枚举
现有兄弟目录则会对新建目录、重命名和链接产生额外语义，不能冒充“workspace 除某子目录外均可写”。
ABI 2 起 `REFER` 可控制跨目录 link/rename，ABI 3 起可控制 truncate，但这不改变上述规则组合方式；
见[文件系统权限列表](https://docs.kernel.org/6.17/userspace-api/landlock.html#filesystem-flags)。

此外，Landlock 只检查安装规则之后打开/解析的对象：安装前已经打开的文件描述符不受这些文件访问
限制；若要形成完整边界，执行前必须裁剪继承 fd。参见
[Linux 6.17 文件系统 flags 说明](https://docs.kernel.org/6.17/userspace-api/landlock.html#filesystem-flags)和
[特殊文件系统限制](https://docs.kernel.org/6.17/userspace-api/landlock.html#special-filesystems)。

#### 5.2 网络与 Unix socket

ABI 4–7 的 Landlock 网络规则对象是**端口号**，能力只有 `LANDLOCK_ACCESS_NET_BIND_TCP` 和
`LANDLOCK_ACCESS_NET_CONNECT_TCP`。它不能按 IP、域名或进程级出口代理表达授权，也不覆盖 UDP；
见[Linux 6.17 network flags](https://docs.kernel.org/6.17/userspace-api/landlock.html#network-flags)。
因此它能实现“TCP 全禁/只准某些端口”，不能实现计划要求的“只准某个域名且必须经过受控出口”。

ABI 6 起的 `LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET` 只阻止连接到 Landlock domain 外创建的 abstract
Unix socket；它不接受例外规则，而且同域或子域创建的 socket 仍可连接。具体语义见
[IPC scoping](https://docs.kernel.org/6.17/userspace-api/landlock.html#ipc-scoping)。本机 ABI 7 尚无后续
ABI 才加入的 pathname Unix socket resolve 控制；`LANDLOCK_ACCESS_FS_MAKE_SOCK` 只能限制在目录中
创建 socket 文件，不能据此宣称已禁止连接既有 pathname socket。继承或通过其他 fd 传递机制获得的
socket fd 也不属于路径规则可重新收回的能力；内核把 pipe/socket 这类非 user-visible filesystem 对象
列为当前限制，见
[special filesystems](https://docs.kernel.org/6.17/userspace-api/landlock.html#special-filesystems)。

## 工程建议（非上游事实）

1. **P01 状态应记为“环境阻塞”，不要把 sandbox-runtime 选成当前可运行后端。** 当前宿主条件下，
   `srt` 只会在同一 namespace 限制处失败；P04/P05 不应因此删除保守审批或开启动态命令自动运行。
2. 若后续由管理员明确解决 user namespace 条件，首版可固定
   `@anthropic-ai/sandbox-runtime@0.0.77`，用独立进程/CLI 适配，不把 Node 库嵌进 C++。每次执行前生成
   完整 settings，并把缺失/无效 settings 视为准备失败；`--control-fd` 仅用于运行中网络列表变化。
3. 采用前必须在改变后的真实环境重新验收：受保护子路径、新建路径、symlink/rename/hardlink、私有临时
   空间、Unix socket/继承 fd、网络代理绕过、取消后的进程树和实际退出状态。当前没有这些通过证据。
4. 若选择“不兼容当前受限主机”，应把“需要 AppArmor `userns` profile 或等效部署条件”写成明确安装
   前置并在启动时 fail closed；不能自动改 sysctl，也不能在 `srt` 失败后回退 full access。
5. 若必须在当前宿主运行，只能继续深化 Landlock + seccomp 的保守路径，并准确报告缺失能力：受保护
   子目录需改成叶子 allow-list 或另加可用隔离原语；域名网络授权需强制代理/出口；pathname Unix
   socket 和继承 fd 需独立处理。能力未补齐前，不应给该后端“完整 workspace sandbox”标签。

## P01 判定

| 项目 | 判定 |
| --- | --- |
| 固定候选版本 | `@anthropic-ai/sandbox-runtime@0.0.77` |
| 许可证 | Apache-2.0，可集成，但须保留许可证与 notices |
| 本机依赖文件 | Node、bwrap、socat、rg 均具备；`srt` 尚未安装 |
| 本机实际可启动 | 否；bubblewrap user/network namespace 准备失败 |
| runtime 内部绕过 uid_map EPERM | 否；强/弱模式均依赖外层 `--unshare-user` |
| Landlock 替代完整能力 | 否；父允许/子拒绝、域名网络、pathname Unix socket、继承 fd 均有缺口 |
| P04/P05 放宽自动执行的前置 | 未满足 |
