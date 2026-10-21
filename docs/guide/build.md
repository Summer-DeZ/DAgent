# 构建与安装

环境要求：Linux，CMake ≥ 3.25、Ninja、支持 C++23 的编译器，Boost ≥ 1.83、libcurl ≥ 7.68 和 OpenSSL Crypto。
首次配置需要联网获取其余第三方依赖。运行时还需要下文列出的沙箱与网页工具依赖。

## 开发构建

```bash
cmake --preset dev                                        # build/dev（Ninja，Debug）
cmake --build --preset dev --target dagent dagent-backend -j 4
```

dev 构建的安装根是源码树 `home/`，`DAGENT_HOME` 可覆盖。

Windows 的 MSVC 开发环境可调用 `cmake --preset win-dev -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake`
（toolchain 路径按实际安装位置传入）；`win-dev` preset 仅在 Windows 宿主可用，配置会照常查找依赖并生成全部目标。

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

首次准备需要联网，普通启动不自动下载或安装。宿主需提供 bubblewrap、socat 和 curl 7.75+（命令行程序，不只是 libcurl）；
Ubuntu 可安装 `bubblewrap`、`socat`、`curl` 包。工具版本与依赖配置见 [toolchain](../design/toolchain.md)。

## AppArmor 主机配置

受限执行只使用 SRT，不提供旧 Landlock 后端或隐式宿主回退。
启用了 AppArmor 非特权 user namespace 限制的主机需安装专用 profile。
开发构建生成的 profile 只匹配本构建目录的 `dagent-backend`：

```bash
sudo install -m 644 build/dev/dagent.apparmor /etc/apparmor.d/dagent-backend
sudo apparmor_parser -r /etc/apparmor.d/dagent-backend
build/dev/src/dagent sandbox status
```

确认 `probe.ok=true` 后正常启动 `dagent`，无需 `aa-exec`。
profile 只授予 SRT 启动所需的 userns 能力；实际文件和网络边界仍由 SRT 实施。
不关闭全局 AppArmor 限制。已运行的后端需要退出后重新启动。
未启用 AppArmor 的主机不需要安装此 profile，仍需通过真实 SRT 探测。

```bash
build/dev/src/dagent
```

## 正式安装

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target dagent dagent-backend
cmake --install build/release --prefix <安装根>
```

安装补充缺失的用户资源，更新二进制、SRT bridge 和 SearXNG 启动入口，并生成 profile。
非 dev 构建的 Home 是可执行文件所在目录，`DAGENT_HOME` 可覆盖。
`dagent` 与 `dagent-backend` 必须在同一目录。目录布局见 [home](../design/home.md)。
目标主机需要与编译器兼容的 C++ 运行库。开发构建可能通过 build RPATH 使用工具链自带的 libstdc++，
安装时会移除该路径；若系统库较旧，应安装兼容运行库或在启动环境中配置相应库路径。

安装时生成 `<安装根>/libexec/dagent.apparmor`，匹配最终安装路径（不含 `DESTDIR`）。
在上述主机上用该文件替代开发 profile 安装并加载，再从正式安装路径运行
`dagent sandbox status`。更换安装路径后需重新生成并加载对应 profile。

正式安装也要在目标 Home 执行 `runtime sync`；不要把开发 Home 的凭据、数据库或已准备 runtime 当作发行资源。
已有配置不会被安装覆盖，升级前应核对配置版本，当前 `sandbox.version` 为 2。
这些步骤描述目录安装，目前未提供 `.deb` 或独立 `/usr/bin` 布局。

决策模型的固定阈值配置位于 `config/decision/`，随 Home 一起安装。安装模板仍将决策模型设为禁用；
启用及升级配置见 [决策模型代批](permissions.md#决策模型代批auto-review)。已有配置不会自动合并新的阈值文件设置。

## 升级已有 Home 的 web 资源

网页模块使用 FetchContent 固定的 lexbor v3.0.1，字符集转换使用 glibc iconv。
`runtime sync` 准备 `internal/searxng`：固定源码 commit 与 SHA-256、独立托管 Python 环境、带哈希的 wheel 锁文件。
不需要 Docker 或系统 SearXNG 服务。已有 Home 的配置、提示词和锁文件不会被安装自动覆盖，升级时按以下步骤合并：

1. 安装同版本的前后端二进制及 `libexec/srt_bridge.mjs`、`libexec/srt_bwrap.sh`、`libexec/searxng_server.py`。这些入口始终位于二进制旁，
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
