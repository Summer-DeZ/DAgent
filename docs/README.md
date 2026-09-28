# DAgent

DAgent 是用 C++23 编写的终端 Agent，仅支持 Linux。前端 `dagent` 提供命令行与交互界面，并独占启动后端
`dagent-backend`，由后端负责配置、模型、工具与会话记录。

## 使用

```bash
dagent runtime sync        # 首次配置模型后准备工具环境
dagent                     # 交互界面
dagent run "提示词"         # 非交互一轮；--output text|json|jsonl
dagent sessions            # 当前目录最近的会话
dagent --list-models
```

配置、提示词与主题位于安装根（默认为可执行文件所在目录，`DAGENT_HOME` 可覆盖），首次配置见 [home](design/home.md)。

## 构建

环境要求：

- Linux
- CMake ≥ 3.25、Ninja、支持 C++23 的编译器
- Boost ≥ 1.83、libcurl、OpenSSL Crypto
- libseccomp：`apt install libseccomp-dev`
- 首次配置需要联网，CMake 会拉取其余第三方库

构建与安装步骤见 [构建与安装](guide/build.md)。
