# 构建与安装

环境要求：Linux，CMake ≥ 3.25、Ninja、支持 C++23 的编译器，Boost ≥ 1.83、libcurl ≥ 7.68 和 OpenSSL Crypto。
首次配置需要联网获取其余第三方依赖。运行时还需要下文列出的沙箱与网页工具依赖。

## 开发构建

```bash
cmake --preset dev                                        # build/dev（Ninja，Debug）
cmake --build --preset dev --target dagent dagent-backend -j 4
```

dev 构建的安装根是源码树 `home/`，`DAGENT_HOME` 可覆盖。

## Windows

先决条件：Visual Studio 2022 Build Tools（MSVC C++ 工作负载与 Windows SDK）、
vcpkg（`boost-asio`、`boost-process`、`curl`（默认 ssl 特性即 schannel）、`openssl`、`libiconv`，triplet `x64-windows`）
和 Git for Windows（`runtime sync` 使用系统安装的 `git.exe` 与 `bash.exe`）。

```powershell
cmake --preset win-dev -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset win-dev
```

`win-dev` 与 `win-release` preset 仅在 Windows 宿主可用，均使用 Ninja 和 `x64-windows` triplet；
toolchain 路径按 vcpkg 实际安装位置传入。`win-dev` 的 Home 是源码树 `home/`，
`win-release` 不设置 `DAGENT_DEV_HOME`，Home 默认为可执行文件所在目录：

```powershell
cmake --preset win-release -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset win-release
cmake --install build/win-release --prefix C:/DAgent
```

安装目录包含 `dagent.exe`、`dagent-backend.exe` 及它们的运行时 DLL，即 vcpkg 的 Boost.Process、
Boost.Filesystem、libcurl、libcrypto、iconv、zlib 对应配置。系统 DLL 与 VC 运行库不会被收集；
目标机需安装与构建工具链匹配的 Microsoft Visual C++ Redistributable。Windows 不安装 `libexec`
（SearXNG 启动入口与 AppArmor profile 仅 Linux 提供）。

正式安装的 Home 首次仍需执行 `dagent runtime sync`，它使用系统 Git for Windows 的 git/bash，
并下载 node、uv、rg 的 Windows 归档和 `mxc` 包（`@microsoft/mxc-sdk@1.0.1` 中的 `mxc_ffi.dll`）。网络受限时可先把归档按 SHA-256 放到
`home/cache/<sha256>.archive`，`runtime sync` 校验后直接使用，不再下载。
Windows 的沙箱后端是 ProcessContainer，需要 Windows 11 26H2（build 26300）或更新：`dagent sandbox status` 加载 `mxc_ffi.dll`、
报告 `mxc-tier`（只接受 `base-container`）并做真实探测。目前 Git for Windows 的 bash 无法在 ProcessContainer 内初始化
（microsoft/mxc#1061），探测在 filesystem 阶段失败并给出 `host_prerequisite`，DAgent 以 ask 模式启动、不提供 workspace 模式；
该问题修复前 Windows 没有受限执行。客户机经代理下载 npm 包超时时，把 tgz 按 SHA-256 预置到 `home/cache/packages/<sha256>.archive`。

## 首次准备

开发目录缺少模型或 MCP 配置时，从示例创建，保留已有文件：

```bash
if [ ! -f home/config/models.json ]; then
  install -m 600 home/config/models.example.json home/config/models.json
fi
if [ ! -f home/config/mcp.json ]; then
  install -m 600 home/config/mcp.example.json home/config/mcp.json
fi
```

编辑模型地址、模型 ID 与认证，按需配置 MCP；示例中的模型地址不代表对应服务正在运行。
随后准备工具环境：

```bash
build/dev/src/dagent runtime sync
build/dev/src/dagent runtime list
```

首次准备需要联网，普通启动不自动下载或安装。`runtime sync` 下载 MXC 原生库（runtime 的 `mxc` 包，npm 上
`@microsoft/mxc-sdk@1.0.1` 的 tgz，按 SHA-256 校验），二进制不进仓库；后端直接加载它，不经过 Node。
准备依赖环境时，`uv venv`/`uv pip sync`/`npm ci` 会收到宿主的 `HTTP_PROXY`、`HTTPS_PROXY`、`ALL_PROXY`、`NO_PROXY`
及其小写形式，只能经代理出网的主机照常设置这些变量即可。工具版本与依赖配置见 [toolchain](../design/toolchain.md)。

受限执行由 MXC 的 bubblewrap 后端落实，宿主需提供：

| 依赖 | 用途 |
| --- | --- |
| bubblewrap ≥ 0.5 | 文件与进程隔离（MXC 用到 `--clearenv`） |
| util-linux（`unshare`、`nsenter`、`setsid`） | 代理模式的网络命名空间；`setsid -w` 启动受限 shell |
| slirp4netns | 代理模式下沙箱唯一的网络出口 |
| iproute2（`ip`） | 代理模式检查沙箱 loopback |
| iptables（nf_tables 后端，含 `iptables-restore`、`ip6tables-restore`） | 在沙箱的网络命名空间里只放行代理端点 |
| 已加载的 `nf_conntrack` 内核模块 | 入站链的连接状态匹配；非特权 bubblewrap 无法按需加载 |
| glibc ≥ 2.34 | 预编译的 `libmxc_ffi.so` 引用 GLIBC_2.34 符号 |
| curl 7.75+（命令行程序，不只是 libcurl） | web_fetch / web_search 在沙箱内发请求 |

Ubuntu 可安装 `bubblewrap`、`slirp4netns`、`util-linux`、`iproute2`、`iptables`、`curl` 包；`iptables --version`
应显示 `(nf_tables)`，否则用 `update-alternatives --set iptables /usr/sbin/iptables-nft`（ip6tables 同理）切换；
`lsmod | grep nf_conntrack` 没有输出时用 `sudo modprobe nf_conntrack` 加载。
不再需要 socat，也没有 Node 的 sandbox 环境。`dagent sandbox status` 检查 bwrap 版本以及 slirp4netns、unshare、nsenter、setsid、
ip、iptables-restore 是否存在；ip6tables、nf_tables 后端与 nf_conntrack 由 MXC 在代理模式启动前校验，问题体现在 `probe` 的 `network` 阶段。

## AppArmor 主机配置

受限执行只使用 MXC，不提供旧 SRT、Landlock 后端或隐式宿主回退。
MXC 原生库在 `dagent-backend` 进程内加载，bwrap、unshare、slirp4netns 都由后端直接生成，因此只需让后端本身获得 userns 能力。
启用了 AppArmor 非特权 user namespace 限制的主机需安装专用 profile。
开发构建生成的 profile（`build/dev/dagent.apparmor`）只匹配本构建目录的 `dagent-backend`：

```bash
sudo install -m 644 build/dev/dagent.apparmor /etc/apparmor.d/dagent-backend
sudo apparmor_parser -r /etc/apparmor.d/dagent-backend
build/dev/src/dagent sandbox status
```

确认 `probe.ok=true` 后正常启动 `dagent`，无需 `aa-exec`。
profile 对后端路径是 `flags=(unconfined)` 加 `userns`，子进程无转换规则因而继承；实际文件和网络边界仍由 MXC 实施。
不关闭全局 AppArmor 限制。已运行的后端需要退出后重新启动。
未启用 AppArmor 的主机不需要安装此 profile，仍需通过真实的 MXC 探测（`probe` 的 `filesystem` 与 `network` 两个阶段）。
探测失败且错误含 namespace、uid_map、loopback 或 `unshare failed` 时，`sandbox status` 的 `host_prerequisite` 会提示部署该 profile。

```bash
build/dev/src/dagent
```

## 正式安装

以下步骤描述 Linux 布局；Windows 的 preset、DLL 与运行库要求见上一节。

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target dagent dagent-backend
cmake --install build/release --prefix <安装根>
```

安装补充缺失的用户资源，更新二进制和 `libexec/searxng_server.py`，并生成 `libexec/dagent.apparmor`。
安装目录的 `libexec/` 只有这两个文件；MXC 原生库在 Home 的 `runtime/mxc/` 下，由 `runtime sync` 准备。
非 dev 构建的 Home 是可执行文件所在目录，`DAGENT_HOME` 可覆盖。
`dagent` 与 `dagent-backend` 必须在同一目录。目录布局见 [home](../design/home.md)。
目标主机需要与编译器兼容的 C++ 运行库。开发构建可能通过 build RPATH 使用工具链自带的 libstdc++，
安装时会移除该路径；若系统库较旧，应安装兼容运行库或在启动环境中配置相应库路径。

安装时生成 `<安装根>/libexec/dagent.apparmor`，匹配最终安装路径（不含 `DESTDIR`）。
在上述主机上用该文件替代开发 profile 安装并加载，再从正式安装路径运行
`dagent sandbox status`。更换安装路径后需重新生成并加载对应 profile。

正式安装也要在目标 Home 执行 `runtime sync`；不要把开发 Home 的凭据、数据库或已准备 runtime 当作发行资源。
已有配置不会被安装覆盖，升级前应核对配置版本：当前 `sandbox` 段要求 `"version": 3` 与 `"backend": "mxc"`，
旧的 2（SRT）或 1（Landlock）配置会在启动时报配置错误，需要手动改成 3 与 `mxc`，不会静默迁移。
升级旧 Home 时还要把发行版 `runtime.json` 的 `mxc` 包合并进自己的 `config/runtime.json` 再执行 `runtime sync`；
旧的 `internal/sandbox` 环境声明与 `config/sandbox/` 目录已不再使用，可以删除。
这些步骤描述目录安装，目前未提供 `.deb` 或独立 `/usr/bin` 布局。

决策模型的固定阈值配置位于 `config/decision/`，随 Home 一起安装。安装模板仍将决策模型设为禁用；
启用及升级配置见 [决策模型代批](permissions.md#决策模型代批auto-review)。已有配置不会自动合并新的阈值文件设置。

## 升级已有 Home 的 web 资源

网页模块使用 FetchContent 固定的 lexbor v3.0.1，字符集转换使用 glibc iconv。
`runtime sync` 准备 `internal/searxng`：固定源码 commit 与 SHA-256、独立托管 Python 环境、带哈希的 wheel 锁文件。
不需要 Docker 或系统 SearXNG 服务。已有 Home 的配置、提示词和锁文件不会被安装自动覆盖，升级时按以下步骤合并：

1. 安装同版本的前后端二进制及 `libexec/searxng_server.py`。该入口始终位于二进制旁，
   即使通过 `DAGENT_HOME` 把数据目录放在别处也一样。
2. 将发行版 [runtime.json](../../home/config/runtime.json) 中的 `environments["internal/searxng"]` 合并到自己的
   `config/runtime.json`，保留其它工具、Skill、MCP 环境声明。
3. 同步该声明对应的 [requirements.lock](../../home/config/searxng/requirements.lock) 到 Home 的
   `config/searxng/requirements.lock`。以后更新 SearXNG 时，源码 commit/归档哈希和依赖锁文件需成对更新。
4. 按需在 `config/config.json` 添加 `web` 段；省略时使用内置默认值。将新版 system prompt 的网页调研约定合并到自己的提示词，
   保留用户原有指令。子 Agent 是否能用 web 工具，由各自 `agents/*.md` 的工具名单决定。
5. 执行 `dagent runtime sync`、`dagent runtime list` 和 `dagent sandbox status`，然后重新启动后端。
   仅调整 `web.engines` 等行为配置时无需 sync，但同样需重启后端。

`runtime list` 为 current 表示依赖配置与快照一致；`sandbox status` 的真实 probe 确认隔离能力。
搜索引擎能否实际返回结果需运行 `web_search` 确认。使用方式见 [网页指南](web.md)。
本机目录安装仅验证了资源交付和配置可读，完整跨主机升级/卸载仍见 [验收边界](../archive/2026-10-05-web-tools-plan.md#验收边界)。
