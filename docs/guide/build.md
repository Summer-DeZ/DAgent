# 构建与安装

环境要求见 [README](../README.md#构建)。

## 开发构建

```bash
cmake --preset dev                                        # build/dev（Ninja，Debug）
cmake --build --preset dev --target dagent dagent-backend # 输出在 build/dev/src/
build/dev/src/dagent
```

dev 构建的安装根是源码树 `home/`，`DAGENT_HOME` 可覆盖。

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

## 正式安装

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target dagent dagent-backend
cmake --install build/release --prefix <安装根>
```

安装只补充缺失的资源。非 dev 构建的安装根是可执行文件所在目录，`DAGENT_HOME` 可覆盖。
`dagent` 与 `dagent-backend` 必须在同一目录。目录布局见 [home](../design/home.md)。

安装时生成 `<安装根>/libexec/dagent.apparmor`，匹配最终安装路径（不含 `DESTDIR`）。
在上述主机上用该文件替代开发 profile 安装并加载，再从正式安装路径运行
`dagent sandbox status`。更换安装路径后需重新生成并加载对应 profile。
