# Linux 安装与打包建议

日期：2026-09-28。状态：设计建议，尚未实现或验证安装包。本次仅阅读源码、现有 ELF 和官方资料，没有修改程序、构建或运行模型。

版本边界（2026-09-30）：下文的 ELF 依赖和行号是 9 月 28 日快照。当前版本已移除 DAgent 的
libseccomp 构建依赖，新增随程序分发的 SRT bridge 与按后端路径生成的 AppArmor profile。
现行目录安装见 [构建指南](../guide/build.md)；本文提出的系统 bin/libexec 分离、CPack 和 `.deb` 尚未落地。

建议先支持 Ubuntu 24.04 的 amd64、arm64，使用 CMake install + CPack 产出 `.deb` 和有明确系统依赖的 `.tar.gz`。先分离程序安装位置和用户 Home，再扩展 RPM。发行版与架构组合只有经过真实运行后才能列为已支持。

## 已核实的项目现状

| 事实 | 对打包的影响 | 源码依据 |
| --- | --- | --- |
| 正式构建默认 Home 是真实可执行文件所在目录，且必须可写 | 直接放入 `/usr/bin` 后，普通用户无法按默认路径启动 | [paths.cpp:16–48](../../src/private/app/paths.cpp#L16) |
| 配置、资源、会话、日志和托管工具均从同一个 Home 派生 | 应保留统一用户 Home，但不能再等同安装前缀 | [home.hpp:14–20](../../src/public/app/home.hpp#L14) |
| 两个可执行文件安装到 prefix 根，前端从同目录寻找后端 | 采用标准 bin/libexec 布局需同时修改后端定位 | [CMakeLists.txt:23–25](../../CMakeLists.txt#L23)、[channel.cpp:116–122](../../src/private/ipc/channel.cpp#L116) |
| 安装脚本仅复制缺失资源，跳过真实 models/mcp 配置 | 适合个人目录初始化，不应同时承担包内资源更新和用户配置保护 | [install_home.cmake.in:4–19](../../cmake/install_home.cmake.in#L4) |
| dev preset 编入源码 Home 路径 | 必须全新 Release 构建，不能分发 dev 二进制 | [CMakePresets.json:5–10](../../CMakePresets.json#L5) |
| Git helper/template 来源为固定系统路径 | 当前 runtime 配置不能直接宣布跨发行版通用 | [runtime.json:15–26](../../home/config/runtime.json#L15) |
| runtime manifest 存实际绝对路径，Python 环境不能直接搬家 | 不打包开发机已准备的 runtime，安装后由用户重新 sync | [toolchain.cpp:226–232](../../src/private/app/toolchain.cpp#L226)、[执行环境与边界](../design/toolchain.md#执行环境与边界) |

当前机器为 Ubuntu 24.04/aarch64。现有 `build/dev/src/dagent-backend` 的直接动态依赖包含 libseccomp、libcurl、libcrypto、libstdc++、libm、libgcc_s、libc；版本需求最高为 `GLIBC_2.38` 和 `GLIBCXX_3.4.32`。这些是已有产物的观测，不能代表未来 Release 的兼容承诺。已有 release 后端未列出当前源码使用的 libcrypto，也不能直接拿来发布。

## 推荐布局与职责

```text
<prefix>/
├── bin/dagent
├── libexec/dagent/dagent-backend
└── share/dagent/defaults/
    ├── config/                 干净的发布配置与模型/MCP示例
    ├── prompts/
    ├── agents/
    ├── skills/
    └── themes/

<DAGENT_HOME>/                   当前用户拥有，沿用现有内部结构
├── config/
├── prompts/ agents/ skills/ themes/
├── data/dagent.db
├── logs/ run/
└── runtime/ cache/
```

DEB 使用 `/usr` 前缀；用户自行安装可以使用 `~/.local`。目录命名使用 `GNUInstallDirs`，install destination 保持相对路径，便于 `--prefix` 和 staging。[CMake 3.25 GNUInstallDirs](https://cmake.org/cmake/help/v3.25/module/GNUInstallDirs.html)

职责建议：安装路径对象只定位后端和默认资源；现有 `HomePaths` 继续负责全部用户状态；初始化操作负责从默认资源创建用户 Home。后端实际启动路径由应用装配传入 IPC 层，避免 IPC 层自行承担安装布局知识。正式构建按真实可执行文件和配置的相对安装布局定位资源，不能编入源码路径；开发构建保留明确的 dev 布局。

默认用户 Home 建议为 `$XDG_DATA_HOME/dagent`，未设置时为 `~/.local/share/dagent`，`DAGENT_HOME` 优先覆盖，dev 默认保持仓库 Home。这里借用 XDG 数据根的默认位置，继续把配置与状态集中在一个目录；这是保持项目现有模型的取舍，并非完整的 XDG 配置/缓存/状态拆分。[XDG Base Directory Specification](https://specifications.freedesktop.org/basedir/latest/)

## 首次安装、升级与卸载

建议新增 `dagent home init`，在完整配置和模型初始化之前处理；这是待实现命令。它创建 Home、复制缺失的默认资源、设置 models/mcp 为 0600，不下载依赖、不联系模型、不覆盖已有文件。没有 Home 时，普通启动应显示明确的初始化说明。

预期用户流程：安装包 → `dagent home init` → 编辑模型配置 → `dagent runtime sync` → `dagent`。无需用户安装 C++ 编译工具链。包管理器阶段只安装程序与默认资源，不代替用户运行 sync。

包升级可以更新程序及 `share/dagent/defaults`，用户 Home 保持原样。首版保留显式更新配置/提示词的方式：发布说明给出必要变更，不自动合并，不静默补业务参数。这个取舍意味着用户已有提示词不会随程序自动更新；若新版本要求额外配置，应清楚报告对应字段和更新说明。现有只补缺的复制逻辑属于 Home 初始化，不能用于阻止包内默认资源更新。

卸载只移除包拥有的文件，保留 Home 中的凭据、配置和会话。后端继续由前端按需启动，沿用现有私有进程模式，无需为了打包引入常驻 systemd 服务。[backend_main.cpp:2](../../src/private/app/backend_main.cpp#L2)

## 依赖与兼容范围

保留当前第三方库静态构建、系统基础库动态链接的方向。构建依赖与用户运行依赖分开列出，不因为源码要求 Boost/CMake 就让安装包依赖开发工具。DEB 启用 `CPACK_DEBIAN_PACKAGE_SHLIBDEPS`，从最终 ELF 生成共享库依赖；显式补充实际外部命令及证书依赖，例如 bash、git、tar、gzip、xz-utils、ca-certificates。这些外部命令不会仅凭 ELF 扫描自动完整发现。[CMake 3.25 DEB generator](https://cmake.org/cmake/help/v3.25/cpack_gen/deb.html#variable:CPACK_DEBIAN_PACKAGE_SHLIBDEPS)、[toolchain.cpp:246–266](../../src/private/app/toolchain.cpp#L246)

在选定基线环境分别构建 amd64/arm64；最终以新 Release ELF 的符号版本、依赖包及真实运行结果确定要求。压缩包也携带相同的系统依赖说明，不把更换容器格式当作 ABI 兼容方案。Node/Python 等下载工具自身也有系统要求，必须包含在完整运行验收中。

先保持 Ubuntu 对应 runtime 配置可用；扩展发行版时，用受控的宿主 Git 探测或对应发行版配置消除固定 helper 路径。Git 官方支持 `git --exec-path` 查询 helper 路径，但模板路径需单独处理，不凭 helper 路径猜测。[Git 官方文档](https://git-scm.com/docs/git#Documentation/git.txt---exec-pathltpathgt)

安装包只包含两个程序、明确列举的默认资源、文档和第三方声明。真实凭据、数据库、日志、cache、runtime 不进入包；不能直接归档当前工作目录的整个 `home/`。发布用配置应来自独立可审查的资源清单，避免把开发时修改的配置或新增 JSON 自动收入包。

## 产物与落地顺序

| 产物 | 目标 | 首期范围 |
| --- | --- | --- |
| `.deb` | 包管理器安装、依赖解析和升级 | Ubuntu 24.04，分别构建 amd64/arm64；Debian 另行验证 |
| `.tar.gz` | 无管理员权限、手动安装 | 同一系统基线与依赖要求，支持指定 prefix；不携带现成 runtime |
| `.rpm` | Fedora/RHEL 家族 | 后续选择明确发行版基线，完成 Git 路径与依赖适配后增加 |

CPack 3.25 已有 [DEB](https://cmake.org/cmake/help/v3.25/cpack_gen/deb.html)、[TGZ](https://cmake.org/cmake/help/v3.25/cpack_gen/archive.html) 和 [RPM](https://cmake.org/cmake/help/v3.25/cpack_gen/rpm.html) generator，无需另写一套归档工具。所有产物共享安装资源清单，依赖元数据按发行版生成。

1. 分离安装资源路径与用户 Home；调整后端定位；实现显式 Home 初始化。
2. 改造 install 规则和发布资源清单；加入 Release preset、CPack 配置。为 DAgent 指定安装 component，避免第三方开发头文件/静态库混入运行包。
3. 固定发行版与架构构建环境，收集依赖、版本、发布者信息、许可文件和校验和；按同一版本发布两个二进制。
4. 在目标系统完成普通用户首次安装、runtime sync、真实模型请求、会话保存/恢复、终端交互与受限工具运行；确认沙箱能力在目标内核上实际可用。
5. 实际升级和卸载，确认资源更新与用户 Home 保留；压缩包安装另检查重定位后的后端/资源发现。检测材料仅置于 `temp/`，不新增测试程序、模拟模型或测试构建目标。

根 CMake 当前仍启用 `test` 子目录（[CMakeLists.txt:27–28](../../CMakeLists.txt#L27)），正式实施时需按项目禁止测试代码/目标的要求处理；本次方案研究未删除或修改现有文件。

当前无需先做 AppImage、Flatpak 或独立的在线安装器。先把路径、首次初始化、依赖和升级语义收敛，便能用现有 CMake 工具链完成可维护的首版 Linux 发布。
