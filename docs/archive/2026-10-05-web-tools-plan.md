# web_search / web_fetch：沙箱内的联网工具

归档日期：2026-10-05。原位置：`nexttodo/web-tools-plan.md`。实现基线：`eec9829`。
保留设计决策和当日本机验收记录；当前使用方法见 [网页指南](../guide/web.md)，实现契约见 [web](../design/web.md)，
剩余覆盖范围见 [后续工作](../../nexttodo/README.md)。本次文档归档未重跑功能场景，原始材料仍在 `temp/web-tools/`，不随仓库分发。

设计日期：2026-10-05。状态：已实现，七项验收场景在本机通过；审批由正式 RPC 接口回答，终端验证范围及其它边界见第 10 节。

## 1. 目标

- `web_search`：查询由 harness 托管的 SearXNG，返回标题、URL、摘要。
- `web_fetch`：获取 http/https 页面。HTML 转成 Markdown（链接改为绝对地址），文本和 JSON 原样返回，长页面按 offset 分页。
- 硬约束：两个工具的出网都受沙箱管理，并且和 bash 共用同一套网络权限；SearXNG 跟随 harness 的生命周期，不依赖系统服务。

不做：厂商原生搜索、JS 渲染、PDF/图片、robots.txt、用小模型摘要页面、web 调用并行执行，以及 SRT 不可用时退回宿主执行。

## 2. 总体思路

1. **出网在只读 SRT 内完成**：工具在只读沙箱里调用 curl。每个连接（包括重定向的每一跳）都会触发 SRT 代理回调，
   并走 bash 现有的运行中网络闸门：配置 `denied_targets` → 会话拒绝 → 父/会话允许 → 配置 `allowed_targets` → 审批。
   审批框、`host:port` 会话规则、`/permissions` 撤销与活跃执行终止、父模型审阅、每次执行的请求预算全部复用，不另建一套网络权限。
2. **核心只加一个意图类型 `ToolKind::network`**：Policy 只判断这次调用能否进入只读 SRT，真正决定能连哪里的是运行中闸门。
   不复用 `external`（它按工具名授权，套不上目标规则）或 `read`（会被放进并行组，而并行组里的网络判定是空的）。
3. **外围新增 `dagent_web` 模块**：负责沙箱内传输、字符集解码、HTML→Markdown、SearXNG 协议。不含权限，也不含给模型看的措辞。
4. **工具层**：新增两个工具，外加 Context 级的页面缓存，用于翻页时不重复下载。把 Grant→SRT 请求的映射从 bash 里抽出来共用，
   这层映射就是安全边界，不能复制两份。
5. **SearXNG 由 harness 托管**（见第 4 节）。

## 3. 权限语义

| 生效权限 | 能否调用 | 连接目标 |
| --- | --- | --- |
| ask / workspace | 可以，只读 SRT | 按闸门顺序判定，需要时弹审批 |
| unrestricted | 可以 | 配置拒绝和会话拒绝仍然生效，其余直接放行（与 bash 宿主执行可以任意联网保持一致） |
| read_only / plan | 可以（调研本来就是规划的一部分） | 同 ask；不能弹审批的子 Agent 只能访问已放行的目标 |
| SRT 只读能力缺失 | 拒绝 | — |

- 会话规则与 bash 共用：在 web_fetch 里批准了 `github.com:443`，bash 里的 git 访问同一目标也不再询问，反之亦然。
- 审批框显示工具名、目标，以及搜索词或 URL 摘要。
- 用户明确拒绝时终止本次调用，并把该目标记为会话拒绝；命中配置拒绝时只让那个连接失败，工具会报告是哪个目标、因为什么被拒。
- 从 unrestricted 降级时，只靠 unrestricted 放行的目标会重新判定，正在运行的调用因此被终止。

## 4. SearXNG 跟随 harness

- **安装**：纳入现有 runtime 管理，由 `dagent runtime sync` 准备，不用 docker。SearXNG 源码使用固定 commit 的归档（带 SHA-256），
  依赖放在独立的 `internal/searxng` Python 环境里（托管 Python 加带哈希的锁文件，只装预编译 wheel），和 `internal/sandbox` 同级。
- **配置**：settings.yml 由 DAgent 在 run 目录生成：开启 JSON 格式、关闭限流、密钥随机、只绑定 127.0.0.1。
  用户可以在 Home 配置里覆盖引擎选择。
- **生命周期**：后端在第一次 web_search 时懒启动，通过 `/healthz` 确认就绪；后端退出时随进程组一起结束；进程崩溃后，下一次调用重新拉起。
  做法参照现有 MCP stdio 子进程的管理方式。
- **端点与权限**（已定，2026-10-05）：每个后端各起一个实例，绑定 127.0.0.1 上的动态端口，各后端之间互不共享，
  也不会因端口冲突互相影响。启动时把实际端点交给 web_search，配置里不写端口。这个端点属于 harness 自己的基础设施，
  由 web_search 的授权预先放行，不要求用户写 `allowed_targets`，配置拒绝仍然优先。模型的查询要想出网，仍然必须经过
  「web_search → 托管端点」这一次受管连接。
- **边界**：SearXNG 本身跑在宿主上，不在 SRT 里。原因有二：SRT 隔离了网络命名空间，沙箱里监听的端口从宿主连不进去；
  沙箱内也禁止创建 AF_UNIX socket（S00 实测）。它的地位和模型 API 连接一样，是 harness 基础设施，不属于工具沙箱。

## 5. 技术栈

| 用途 | 选型 | 理由 |
| --- | --- | --- |
| 沙箱内 HTTP | 宿主 curl（在 SRT 内运行） | 代理、重定向、压缩、协议白名单现成可用；不在进程内另写一套 HTTP 权限钩子。新增宿主依赖 curl |
| HTML → Markdown、相对链接解析 | lexbor v3.0.1（C，HTML5 解析 + WHATWG URL，FetchContent 引入） | 活跃维护，自带 CMake；一个库同时解决解析和 URL |
| 字符集 | glibc iconv | 中文 GBK 页面常见；不增加依赖 |
| 搜索 | SearXNG（runtime 托管的 Python 环境） | 自建、免 key、JSON 接口 |
| JSON | 现有 nlohmann | — |

lexbor v3.0.1 已随 `dagent_web` 编译通过，并用于真实 HTML 抓取、相对链接及片段解析。

## 6. 已实测的约束（2026-10-05，真实 SRT 0.0.77）

- 沙箱内的 `NO_PROXY` 含 `127.0.0.1` 和私网段，直连会失败，所以 curl 必须带 `--noproxy ''`。加上以后，本机端点和私网地址也会进入审批。
- 重定向每一跳都会单独回调；SRT 自己不缓存决定，去重靠 Dispatcher 现有的「每次执行」去重表。
- 被拒的明文 HTTP 请求只表现为代理返回 403、curl 退出码 0，所以拒绝必须由工具在闸门回调里自己记录。
- `--max-filesize` 遇到超限会整页放弃，改用 `| head -c` 截断。截断后 curl 的元数据（状态、类型、最终 URL、退出码）仍然完整。
- 代理会拒绝「解析到回环/本机地址的主机名」，这正好是 web_fetch 需要的 SSRF 防护。
- bridge 启动约 0.1–0.3 s；HTTPS 在只读沙箱内可用（系统 CA 可读）。

## 7. 改动范围（模块级）

| 位置 | 内容 |
| --- | --- |
| `dagent_web`（新） | 沙箱内 GET、解码、HTML→Markdown、SearXNG 请求与解析 |
| 核心 agent | `ToolKind::network`；Policy 的判定与 unrestricted 下的目标放行；运行中审批的原因文案；`WebView` 及其记录编码 |
| tools | web_search、web_fetch；页面缓存；共用的 Grant→SRT 映射（bash 一起改用） |
| app / runtime | `web` 配置段；`internal/searxng` 环境；SearXNG 进程的托管与健康检查 |
| UI / 提示词 | 搜索/抓取卡片；plan 模式允许 web 工具；不可信内容的使用约定 |
| 文档 | tools、agent §7、toolchain、permissions、build（curl）及 web 设计文档 |

## 8. 验收（真实运行）

1. ask 模式搜索：自动拉起 SearXNG，返回结果；后端退出后进程随之消失。
2. web_fetch 新域名会弹出 `web_fetch wants to connect to host:443`；选「本会话允许」后，bash 访问同一目标不再询问。
3. 有重定向的站点逐跳询问；拒绝后本会话不再弹窗。
4. 配置拒绝在 unrestricted 下仍然生效；unrestricted 下新域名不弹窗，降级后正在运行的调用被终止。
5. plan 模式两个工具都可用；headless 模式下未放行的目标报「无审批器」。
6. 长页面 offset 翻页时命中缓存，不再联网；GBK 页面没有乱码。
7. SearXNG 被杀掉后，下一次搜索自动恢复；SRT 不可用时两个工具都被策略拒绝。

## 9. 已知限制

- web 调用只能串行：并行组里无法审批。以后要并行，需要给并行组提供一个「只查规则、遇到需要询问就拒绝」的判定，属于核心改动。
- 超时包含等待审批的时间。
- 不渲染 JS；页面和搜索摘要都是不可信数据，由工具说明约束模型不执行其中的指令。


## 10. 实施与真实验收记录（2026-10-05）

本计划已落地。2026-10-05 的运行使用真实 `dagent` / `dagent-backend`、本地真实模型 `Qwen3.6-35B-A3B`、SRT 0.0.77 和公网目标；
审批操作由 `temp/` 下的协议客户端经正式 JSON-RPC 接口完成。模型生成真实工具调用，工具结果来自正式后端和实际网络连接。没有新增项目测试代码、模拟模型、模拟服务或测试构建目标。
临时协议客户端及原始输出仅位于 `temp/web-tools/`。

### 实施范围

- 新增 `dagent_web`：curl/SRT GET、lexbor HTML/URL、iconv、SearXNG JSON 解析。
- `ToolKind::network` 始终串行进入只读 SRT；`tools::detail::sandbox_request` 是 bash 与 web 共用的授权映射。
- 新增两个工具、Context LRU 页面缓存、WebView 编码/回放和 UI 卡片；plan 提示词明确允许网页调研。
- `internal/searxng` 使用源码 commit `d48c4b555421e824342c51d68482dd0898e54d0f`，归档 SHA-256
  `638cdb4bec31bed24e924c63ab5b343d24a64c6342fedd09dc9eb2bd17dacda8`；托管 Python 独立 venv，依赖哈希锁定且只装 wheel。
- 每个后端独立懒启动、动态端口、健康检查、崩溃重启和进程组清理；异常后端退出由管道 EOF 收尾。
- 配置、构建/安装资源和 tools、agent §7、toolchain、permissions、web 文档已同步。

### 验收结果

| 原验收项 | 结果与证据 |
| --- | --- |
| 1. ask 搜索与生命周期 | 真实搜索返回 SearXNG 官方文档；无需端点审批。两个后端端口分别为 34419 / 40277；退出后进程消失。`search-lifecycle.rpc.jsonl`、`search-independent.rpc.jsonl`、`search-lifecycle.summary` |
| 2. 新目标审批与 bash 共享 | 审批为 `web_fetch wants to connect to docs.python.org:443`；允许后 bash 连接不再网络审批。反向先批准 bash 的 api.github.com，再 web_fetch 也不询问。`permissions.rpc.jsonl`、`formats.rpc.jsonl` |
| 3. 重定向与会话拒绝 | github.com → raw.githubusercontent.com 逐跳询问；拒绝第二跳后终止，下一次直接请求同目标返回会话拒绝，共两次审批。另一次全部允许成功返回最终纯文本 URL。`redirect-deny.rpc.jsonl`、`formats.rpc.jsonl` |
| 4. 拒绝优先与运行中降权 | unrestricted 下 www.python.org 配置拒绝生效；127.0.0.1 配置拒绝也能阻止托管搜索。运行中降为 ask、撤销已使用的 github.com 规则，都使调用被权限终止；对应调用总耗时字段为 562 / 510 ms，不是控制操作后的响应延迟。`config-deny.jsonl`、`search-config-deny.jsonl`、`downgrade.rpc.jsonl`、`revoke.rpc.jsonl` |
| 5. plan 与 headless | plan 中搜索和抓取均实际成功；headless 新域名返回 `runtime network approval is unavailable in this run`。`search-lifecycle.rpc.jsonl`、`headless.jsonl` |
| 6. 缓存、编码和截断 | Python 长文档第二页 `cached=true`、毫秒整数耗时字段为 0、没有 network_targets；央广网 GB2312 页面正确返回中文标题和正文；8 KiB 下载上限保留前缀并报告 truncated。`permissions.rpc.jsonl`、`gbk.jsonl`、`truncation.rpc.jsonl` |
| 7. 崩溃恢复与 SRT 缺失 | 搜索子进程 1876805 被终止后，下次搜索启动 1876997 并返回结果；另一个后端的搜索进程 1877308 独立运行。真实缺少 SRT bridge 的后端副本在 unrestricted 下仍策略拒绝两个工具。`search-lifecycle.summary`、`missing-srt.rpc.jsonl` |

补充：终止本次检测专用后端后，stdin EOF 使其搜索子进程退出，`abrupt-exit.summary` 记录没有存活子进程。
`cmake --preset dev` 及 `cmake --build --preset dev --target dagent dagent-backend -j 4` 通过；
`dagent runtime sync` 成功，`runtime list` 为 current；`sandbox status` 为 probe.ok=true / sandboxing_enabled=true。
临时目录安装实际包含锁文件和两个 libexec 入口，安装后的 `runtime list` 正确读取搜索环境声明。
真实终端 UI 已呈现 `Web fetch` / `HTTP 200` 卡片，证据为 `tui.ansi` 与 `tui.summary`。
以上是本机功能验收，跨架构/跨主机发行升级与卸载仍按 [总后续工作](../../nexttodo/README.md) 单独跟踪。

### 实际发现并修正

- Linux parent-death signal 绑定创建子进程的线程，工具工作线程退出会提前杀死服务；改用后端 stdin 管道 EOF 监听。
- curl 对部分页面的输出检测会中止字节流；显式 `--output -` 后 GB2312 页面成功解码。
- 相对链接转换保留 URL 片段，避免目录链接全部指向文档顶部；下载截断独立于输出分页。
- 运行中权限终止的原因同步到 WebView，卡片和模型结果一致。

### 验收边界

| 已验证 | 尚未覆盖或不能据此推断 |
| --- | --- |
| 七项场景使用真实模型、正式后端、SRT 与实际 HTTP/SearXNG 请求 | 完整并发对抗矩阵、所有网络环境与站点行为 |
| 网络审批请求内容、批准/拒绝、会话复用、撤销、降权由正式 RPC 验证 | 逐个人工点击审批弹窗；本次没有新增 web 子任务与父模型审阅的专项验收 |
| 真实 TUI 显示 Web fetch / HTTP 200 抓取卡片 | 搜索卡片及所有交互状态的逐项人工验证、历史 web 卡片重放的专项运行 |
| SRT bridge 缺失的真实后端副本在 unrestricted 下拒绝两个工具 | 所有可能的 SRT/内核/namespace 启动失败来源 |
| 本机开发 Home 的 runtime sync；临时目录安装包含锁文件和两个 libexec 入口，安装后的 runtime list 可读 | 临时安装目录的完整搜索运行、跨主机/跨架构发行安装、升级和卸载 |
| 权限变化后正在运行的抓取被终止 | 精确的撤销/降权响应延迟；elapsed_ms 是调用总耗时 |

运行时记录里的 `turn_ended.status=done` 只表示该轮正常结束，工具成功还需查看 `tool_finished.data.is_error` 和实际结果。
本次补充核对了这些字段；负向场景的预期结果是明确拒绝或终止，不是 HTTP 成功。

### 上游与功能限制

本机出口下 Yahoo 返回结果，Brave / DuckDuckGo 曾返回 HTTP 错误或验证码。结果会保留可用条目并报告失败引擎，
默认引擎改为 Yahoo、Brave、DuckDuckGo；可通过 `web.engines` 调整。不能把本次成功解释为所有引擎在所有出口长期可用。
正文是静态 HTML 转换，不执行 JS；无 PDF/图片解析；缓存只在当前会话存续并可能被预算淘汰。
