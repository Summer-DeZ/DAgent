# 开发与验收流程

写给接手 DAgent 开发的人（或接手的 Agent）。记录当前实际在用的流程：改动怎么做、在 Linux 上怎么验、在 Windows 虚拟机里怎么验、什么时候跑 CI。构建命令本身见 [构建与安装](build.md)。

## 1. 基本规则

- 项目准则在仓库根目录的 `AGENTS.md`，优先级最高：不写测试代码、模拟模型或演示入口；只用构建和真实运行确认改动；临时检查材料只放 `temp/`；`hpp` 只在 `src/public`，`cpp` 只在 `src/private`；不堆防御性代码；改完不反复做"只看不跑"的 review。
- 设计文档 `docs/design/` 描述当前实现，改了行为或文件布局要同步改。`docs/design/README.md` 是模块索引。
- 计划文档：进行中的计划放 `nexttodo/<name>-plan.md`，只写已批准的目标和决定（提议先在对话里说，批准后再写入）。完成后连同验收范围移到 `docs/archive/<完成日期>-<name>-plan.md`，文件开头加"归档日期 / 原位置"行，并在 `docs/archive/README.md` 的表里登记。
- 分支与提交：每项工作开一个分支（`platform/windows`、`refactor/layout`、`refactor/split-files` 这类命名），一步一个提交，提交信息用 `type(scope): 摘要`；完成后 `git merge --no-ff` 进 `master` 并推送。提交信息不加任何署名或 AI 生成的尾注。
- GitHub Actions 只在手动触发（workflow_dispatch）或推送 `v*` 标签时运行；日常提交不跑 CI，需要跑时先征得项目负责人同意。查看结果用 `gh run list --limit N`、`gh run watch <id> --exit-status`、`gh api repos/<owner>/<repo>/actions/jobs/<id>/logs`。

## 2. Linux：构建与检查

### 2.1 构建

用仓库自带的 CMake（系统 cmake 3.22 读不了 `CMakePresets.json`）：

```bash
CM=build/tools/python/lib/python3.10/site-packages/cmake/data/bin/cmake
$CM --build --preset dev -j 8                       # 全部目标
$CM --build --preset dev -j 8 --target dagent_tui   # 只编一个库，改动小时更快
```

要求零错误零警告：把输出重定向到日志后 `grep -c 'warning:'` 应为 0（配置阶段 CMake 的 policy "warning" 不算）。

需要在另一棵源码树上构建（基线对比、子 Agent 并行开发）时，用 `temp/platform-check/portable_build.sh <源码树> <构建目录>`：它用同一套工具链（`build/tools/sysroot` 的 gcc-14、ninja）、复用 `build/dev/_deps` 的依赖源码，离线配置并安装到 `<构建目录>-install`。产物没有 RPATH，运行时要加 `LD_LIBRARY_PATH=/home/jyt/DAgent/build/tools/sysroot/usr/lib/x86_64-linux-gnu`。

### 2.2 真实运行检查

所有检查脚本在 `temp/platform-check/`，都驱动真实的二进制和真实模型（默认本地 qwen；决策审阅走 d3-local）。

| 工具 | 用途 | 用法 |
| --- | --- | --- |
| `det.sh` | 确定性 CLI 输出（`sandbox status`、`runtime list`、`sessions`、`--list-models`、backend 参数错误等） | `det.sh <绝对输出目录> <程序目录>`，然后 `diff -r` 基线目录；只允许 `elapsed_ms` 和会话列表不同（`sandbox status` 已没有程序目录旁的 bridge 路径） |
| `capture2.py` | 在真实 PTY 里跑 TUI，按步骤发键并保存每步画面帧 | `temp/tui-verify/venv/bin/python temp/platform-check/capture2.py <前缀> <列> <行> '<步骤 JSON>'`；步骤为 `[文本, 等待秒]`，文本可含 `<enter>`、`<esc>`、`<s-tab>`、`<resize:C:R>`、`<signal:TERM>`；环境变量 `DAGENT_BIN`、`DAGENT_HOME`、`CAPTURE_CWD`、`CAPTURE_ARGS` |
| `runs.py` / `f_runs.py` / `g_runs.py` | 用模型跑 `dagent run` 的工具场景（bash、grep、edit、MCP、web、memory、网络…） | `python3 runs.py <标签> [场景...]` |
| `h_rpc.py` / `watch_rpc.py` | 直接用 socketpair 驱动 `dagent-backend` 的 RPC | 见脚本头部 |

非交互运行：`./build/dev/src/dagent run -C <目录> --output jsonl "<提示>" </dev/null`，没有 `</dev/null` 会卡在读 stdin。

临时 Home 在 `temp/refactor-verify/home`（`runtime/` 必须是真实目录，其子项才是符号链接），测试项目在 `temp/refactor-verify/project`；用于会写配置、记忆或 `mcp.json` 的检查。真实 MCP 服务器也在 `temp/refactor-verify/mcp`。

### 2.3 重构类改动的验收套路

纯移动或拆文件这类"不该改变行为"的改动，按下面比对（2026-10-22 的目录整理就是这么验的）：

1. 先在 master 上做基线：全量构建，保存 `build/dev/compile_commands.json`，`cmake --install build/dev --prefix <基线目录>`。
2. 改完后同样构建并安装。纯移动时比对编译命令（按文件基名对应，参数应零差异）和剥离调试信息后的二进制大小与 `nm` 符号表；拆文件时只比对 `dagent::` 命名空间的强符号（`nm --defined-only -C`，类型 T/D/B/R，排除 lambda 与模板实例化），要求没有消失的符号、新增的只有刻意外露的函数。
3. `det.sh` 跑新旧两套产物并 diff。
4. 画面比对：用基线二进制通过 `dagent run` 生成内容丰富的会话（Markdown 标题/列表/表格/代码块、一次 edit 产生的 diff），再用新旧二进制各自 `dagent -r <session>` 回放并用 `capture2.py` 截帧，帧应一致（只有"resumed … N messages"的计数不同）。
5. 涉及权限或审批的改动，再跑一次 ask 模式的 bash 写命令（`CAPTURE_ARGS="--permissions ask"`），确认弹出审批框、`y` 放行后执行并正常 `/exit`。
6. Windows 虚拟机一次 Release 构建加冒烟（见第 3 节）。

## 3. Windows：虚拟机验证

### 3.1 环境

Windows 11 Pro 26H2（build 26300.9711）虚拟机（dockur/windows，容器名 `dagent-win`），全部文件在 `/home/jyt/winvm/`：

- `compose.yml`、`storage/`（磁盘）、`shared/`（在客户机里是 `\\host.lan\Data`）、`tools/`（宿主侧脚本）。
- 连接：`ssh dagent-win`（127.0.0.1:2222，登录 shell 是 PowerShell）；网页控制台 http://127.0.0.1:8006/ ，RDP 127.0.0.1:33890。截图 `tools/shot.sh [out.png]`，注入按键 `tools/type.py`。
- 客户机已装：VS 2022 Build Tools（MSVC、CMake、Ninja、Windows SDK）、Git for Windows、Python 3.12（pywinpty、pyte）、vcpkg 在 `C:\dev\vcpkg`（boost-asio/boost-process、curl、openssl、libiconv，triplet x64-windows）。源码树 `C:\dev\DAgent`，开发用 Home 是源码树的 `home/`，测试项目 `C:\dev\proj 测试`（故意带空格和中文）。
- 网络：客户机只经宿主转发的代理访问 GitHub、微软 CDN、npm 等；Windows Update 不通，不要指望它自我更新。`pip` 用 TUNA 镜像。代理转发器 `socat TCP-LISTEN:1081,bind=172.25.0.1,fork,reuseaddr TCP:127.0.0.1:1081` 不是服务，宿主重启后要重新起（用 `ss -tln | grep 172.25.0.1:1081` 检查）。
- 不保留磁盘备份（2026-10-22 项目负责人决定）；需要回退时按 `compose.yml` 重装并重跑 `oem/` 与 `shared/` 里的安装脚本。

### 3.2 同步与构建

```bash
sha=$(/home/jyt/winvm/tools/win-snapshot.sh <批次名> | tail -1)      # 1. 快照并推送到虚拟机
WIN_BUILD_MAX=3000 /home/jyt/winvm/tools/win-build.sh $sha 1 Release  # 2. 在客户机里构建并等待结果
```

- `win-snapshot.sh` 把 Linux 工作树（含未提交和未跟踪文件，遵守 .gitignore）做成快照提交，强推到客户机仓库的 `refs/sync/wip` 并 detached 检出，打印快照 sha。它会拒绝包含 `home/{data,run,runtime,cache,memory,logs}/`、`credential*`、`.env*`、`.dagent/` 的快照：凭据可以放在虚拟机里，但绝不能经同步传过去。`REPO=<路径>` 可快照另一棵工作树。
- `win-build.sh <sha> [attempt] [Debug|Release] [stages]` 通过 `Invoke-CimMethod Win32_Process Create` 在客户机里脱离 ssh 会话启动 `shared/dagent/win-build.ps1`，等待 `shared/dagent/jobs/<sha>/attempts/NNN/result.json`，打印各阶段状态。默认阶段 `checkout,configure,build,install`；日志在同目录的 `checkout/configure/build/install.raw.log`，`toolchain.json` 记录工具链版本。构建目录 `C:\dev\DAgent\build\win-dev`，安装到 `C:\dev\DAgent-stage`。构建用 `-k 0`，一个编译单元失败不会掩盖其他错误。
- 零警告检查：`grep -c 'warning C' <job>/build.raw.log` 应为 0。
- 冒烟：

```bash
ssh dagent-win 'cmd /c "C:\dev\DAgent-stage\dagent.exe --version & C:\dev\DAgent-stage\dagent.exe sandbox status & echo rc=%ERRORLEVEL%"'
```

当前版本的 Windows 尚未启用沙箱后端（ProcessContainer 接入进行中），`sandbox status` 应输出 `probe.stage="unsupported"` 且 `rc=0`；启动时 TUI 欢迎页和 `dagent run` 的 stderr 都会提示 workspace 模式不可用并以 ask 模式启动。

什么时候需要虚拟机：改了 `*_win.cpp`、CMake 源列表或平台边界（`docs/design/platform.md` 里列的文件）时做一次 Release 构建加冒烟；平台中立的改动靠 Linux 验证即可。虚拟机构建和 Linux 检查并行跑，不要串行等。

### 3.3 TUI 场景

客户机侧的 PTY 捕获工具是 `shared/capture_win.py`（`C:\dev\tools\capture_win.py` 是同一份），用法与 `capture2.py` 相同，但步骤文件通过 `@文件` 传入（ssh → PowerShell → python 的引号转义会把内联 JSON 弄坏），且没有 `<ctrl-t>`。参考 `shared/dagent/jobs/w6/` 的做法：

1. 在宿主写 `jobs/<name>/steps-*.json`（每步 `[文本, 等待秒]`）和 `run.cmd`（用 `printf '%s\r\n'` 写成 CRLF）。`run.cmd` 设置 `DAGENT_BIN`、`CAPTURE_CWD`、`CAPTURE_ARGS`（如 `-m deepseek --permissions workspace`、`--plan`），逐个调用 `python \\host.lan\Data\capture_win.py <前缀> 120 40 @<步骤文件>`，最后复制 `home\logs\*.log` 并写 `result.txt` 作为完成标记。
2. 启动：`ssh dagent-win "Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine='cmd /c \\host.lan\Data\dagent\jobs\<name>\run.cmd'}"`，然后在宿主轮询 `result.txt`。
3. 画面帧在 `jobs/<name>/<前缀>-N.txt`，审批按键是 `y`/`a`/`n`/`e`，切模式 `<s-tab>`。只跑最小场景集合；每套 3–7 分钟。

客户机的 `mcp.json` 等配置改动用同目录的 `*.cmd` 做备份与恢复（如 `restore-mcp.cmd`）。

### 3.4 注意事项

- 长参数经文件传入 `shared/`，不要在 ssh 命令行里嵌 JSON；`(x86)` 这类路径放进 `*.cmd` 再 `ssh dagent-win 'cmd /c \\host.lan\Data\x.cmd'` 执行。
- PowerShell 脚本里跑 git 要 `cmd /c "... 2>&1"`，否则 stderr 的进度输出会被当成错误终止。
- `vcvars64.bat` 会把 `VCPKG_ROOT` 指向 VS 自带的 vcpkg，手工配置时必须显式传 `-DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake`。
- 系统更新后 ssh 偶尔在命令刚结束时报 "closed by remote host"，重试即可；长任务都用后台启动加结果文件，不依赖 ssh 会话存活。
- MSI 安装不要和 VS 安装器同时跑。

## 4. 并行协作

机械但量大的改动（例如把一个文件按职责拆成几个）可以分给多个子 Agent 并行做，每个在自己的 git 工作树里：

```bash
git worktree add temp/wt-<name> -b <topic>/<name> HEAD
```

工作树里没有 `build/`，让子 Agent 用 `portable_build.sh` 同样的配置参数在 `temp/wt-<name>/build/dev` 配置并构建（首次全量约 4 分钟）。分派时写清：只能改自己工作树、禁止对主树的 `build/` 跑 ninja、零警告、提交信息、以及与其他子 Agent 可能重叠的文件（谁先合并谁负责）。全部回来后由主会话按顺序 `git merge --no-ff`，解决 include 行和 CMake 源列表这类小冲突，再做第 2.3 节的整套验收，最后 `git worktree remove` 并删分支。

## 5. 发布

- 打 `v*` 标签并推送会触发 CI（Linux g++-14 dev 预设；Windows vcpkg + MSVC `win-release` 预设、安装、`--version` 与 `sandbox status` 冒烟）。
- Windows 安装包的内容和目标机要求见 [构建与安装](build.md) 的 Windows 一节。

## 6. 常见坑

- zsh：`echo =====X` 会被当成路径展开，用 `'---'`；`--include=*.cpp` 要加引号；`rm -f $J/d-*.txt` 这类 glob 不匹配会中止整条命令；`cd` 会改变后续调用的工作目录，脚本里用绝对路径。
- 不要在同一条 Bash 命令里同时写 `pkill -f <模式>` 和该模式的文本（例如写脚本的 heredoc），会匹配到自己；用 `pgrep -x`/`pkill -x` 按进程名匹配。
- 提示词里 `$` 后跟文字会被解析为技能引用。
- 同一个 `build/dev` 不能同时跑两个 ninja；后台构建进行中不要改它正在编译的文件。
