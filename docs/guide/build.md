# 构建与安装

环境要求见 [README](../README.md#构建)。

## 开发构建

```bash
cmake --preset dev                                        # build/dev（Ninja，Debug）
cmake --build --preset dev --target dagent dagent-backend # 输出在 build/dev/src/
build/dev/src/dagent
```

dev 构建的安装根是源码树 `home/`，`DAGENT_HOME` 可覆盖。

## 正式安装

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target dagent dagent-backend
cmake --install build/release --prefix <安装根>
```

安装只补充缺失的资源。非 dev 构建的安装根是可执行文件所在目录，`DAGENT_HOME` 可覆盖。
`dagent` 与 `dagent-backend` 必须在同一目录。目录布局见 [home](../design/home.md)。
