# 构建与安装

环境要求见 [README](../README.md#构建)。

## 开发构建

```bash
cmake --preset dev                                        # build/dev（Ninja，Debug）
cmake --build --preset dev --target dagent dagent-backend -j 4
```

dev 构建的安装根是源码树 `home/`，`DAGENT_HOME` 可覆盖。

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

安装时生成 `<安装根>/libexec/dagent.apparmor`，匹配最终安装路径（不含 `DESTDIR`）。
在上述主机上用该文件替代开发 profile 安装并加载，再从正式安装路径运行
`dagent sandbox status`。更换安装路径后需重新生成并加载对应 profile。

正式安装也要在目标 Home 执行 `runtime sync`；不要把开发 Home 的凭据、数据库或已准备 runtime 当作发行资源。
已有配置不会被安装覆盖，升级前应核对配置版本，当前 `sandbox.version` 为 2。
这些步骤描述目录安装，不代表已经提供 `.deb` 或独立 `/usr/bin` 布局；后者仍见 [打包设计建议](../research/linux-packaging.md)。

网页模块使用 FetchContent 固定的 lexbor v3.0.1，字符集转换使用 glibc iconv。
`runtime sync` 同时安装 `internal/searxng`：固定源码 commit 与 SHA-256、独立托管 Python 环境、带哈希的 wheel 锁文件。
安装后应具备 `config/searxng/requirements.lock` 和二进制旁的 `libexec/searxng_server.py`；不需要 Docker 或系统 SearXNG 服务。
升级已有 Home 时，把发行配置中的 `internal/searxng` 环境声明合并到 `config/runtime.json` 后执行 sync；安装不会覆盖用户配置。
