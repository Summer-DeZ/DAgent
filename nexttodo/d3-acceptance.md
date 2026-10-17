# d3 实现补充验收

日期：2026-10-17。范围：接收 P1–P8 实现交付后，核对已有运行材料并补做真实 TUI、MCP 和外部文件读取验收。当前结论：P1–P8 已完成全面检查；发现的审批路由、脚本复审、阈值反馈和兼容性缺陷已修复并完成下述真实复验。C1 未开始。

首次补充验收仅记录缺陷；随后应用户要求完成第 3 节修复。使用真实模型服务；交互驱动与原始材料均位于 `temp/d3-acceptance/`。独立 Home 为该目录下的 `home/`，后续 MCP 使用较短的 `temp/m/`，P4 修复验收使用 `temp/f/`；不修改用户主 Home 的模型或 MCP 配置。

## 1. 已完成的补充验收

| 项目 | 真实运行结果 | 原始材料 |
| --- | --- | --- |
| 构建检查 | `cmake --build build/dev -j 2` 成功，Ninja 报告无待编译项 | 本次命令输出 |
| unrestricted 启动默认取消 | 真实 TUI 以 unrestricted 启动，回车选择默认取消，未升权 | `startup-prefix.ansi`、`01-startup-dialog.ansi`、`02-default-cancel.ansi` |
| Shift+Tab 跳过 unrestricted | auto-edit 中打开确认框，再按 Shift+Tab，切到 ask | `03-cycle-dialog.ansi`、`04-shift-tab-skip.ansi` |
| 确认一次并复用 | 选择开启后进入 unrestricted；依次切到 ask、auto-edit，再进入 unrestricted 不重复弹框；独立会话库产生一条 unrestricted_ack | `07-confirm.ansi` 至 `10-reenter-confirmed.ansi`、`tui-permission-results.json` |
| TUI 人工文件审批 | 真实生成模型发起 read，d3 因显式临时 privacy 阈值转人工；在真实审批框按 a 允许本会话 | `tui-read-sequence.ansi`、`tui-read-sequence.json` |
| 同文件复用、另一文件重新审批 | 三轮依次读取 a.txt、a.txt、b.txt；审批框次数为 1、0、1；共两条 outside_read 审阅，最终均记录 exact_file_read / allow_session，目标分别为 a.txt 与 b.txt | 同上；独立 Home 的 dagent.db / decision.sqlite |
| MCP HTTP 真实动作审阅 | Context7 的 resolve-library-id 与 query-docs 均成功；各自一次 d3 审阅批准，authority=decision_model，工具结果 is_error=false | `context7-run.jsonl`、`context7-run.err`、`temp/m/data/decision.sqlite` |

TUI 验收用 pexpect 对真实终端程序发送按键，涉及实际前后端、模型、决策服务和文件操作；未使用模拟模型或模拟服务。这里只确认交互行为与记录，不把 ANSI 捕获等同于人工视觉布局验收。

开发阈值在独立 Home 中使用 `acceptance-temp` profile。人工读取分支将 privacy 临时设为 0，仅用于触发真实审批；这不是正式标定值。主 Home 保持原有 decision 禁用状态；其现存 default 阈值仍是用户交付时注明的开发期临时值。

## 2. 可用的真实 MCP：Context7

选择 [Context7 官方服务](https://github.com/upstash/context7/blob/master/docs/resources/all-clients.mdx)，用于查询库文档，端点为 `https://mcp.context7.com/mcp`。

本次真实探测返回 supportedVersions 包含 `2026-07-28`，serverInfo 为 Context7 4.4.2；随后通过 DAgent 查询 SQLite 官方文档，完成定位库与查询 WAL 资料两次工具调用。此次未提供 API key，不能据此承诺服务长期无鉴权或无限额。

DAgent 配置片段：

```json
{
  "mcpServers": {
    "context7": {
      "type": "http",
      "url": "https://mcp.context7.com/mcp",
      "permissions": {
        "read": [],
        "write": [],
        "network": ["mcp.context7.com:443"]
      }
    }
  }
}
```

包含连接超时设置的完整配置保存在 `temp/d3-acceptance/context7-mcp-config.json`。本次只接入临时 Home，没有写入主 `home/config/mcp.json`。

这次验证覆盖 HTTP 工具发现、tools/call、MCP 动作 d3 审阅与结果返回；未覆盖有远端副作用的 MCP 操作或 MCP stdio 成功路径。

## 3. P4 已修复：bash 外部读取审批被覆盖

真实复现条件：

- workspace：`/home/jyt/DAgent/temp/d3-acceptance/workspace`。
- 目标文件：`/home/jyt/DAgent/temp/d3-acceptance/outside/a.txt`，文件真实存在，read 工具已成功读取。
- auto-edit，决策模型启用，使用显式临时 outside_read 阈值。
- 真实生成模型按要求调用 bash：`cat /home/jyt/DAgent/temp/d3-acceptance/outside/a.txt`。

修复前实际结果：没有 auto_review 或读取审批，tool_started 的 source=mode、authority=user、sandbox=workspace_write；readable 未追加目标文件，命令 exit code 1。沙箱仍阻止了这次未授权读取，但计划要求的审批与执行流程没有完成。

落点：`src/private/agent/permission.cpp` 的 `Policy::evaluate()`。

1. `cmd.known_readonly` 分支发现外部文件后，将 verdict 置 ask，再 break 离开 switch。
2. 没有命中会话授权后，函数末尾 workspace 对所有 exec 无条件返回 workspace_write/allow。
3. 上面的 exact_file_read 审批因此被覆盖，Dispatcher 不会进入 d3 审阅。

已实施修复：保留已有会话文件规则匹配，将 workspace 的 exec 自动放行限定为 dynamic_command 请求，不覆盖 exact_file_read 或敏感读取审批；随后由现有 grant_for(exact_file_read) 保持 read_only 并追加精确文件范围。

修复审批路由后，真实获批读取又暴露了 `exec::sensitive_paths()` 对单文件使用目录迭代器的问题，报 `Not a directory`。现已按文件类型分流：单文件只检查自身敏感名，目录继续沿原逻辑遍历。对应源码为 `src/private/agent/permission.cpp`、`src/private/exec/sandbox.cpp`，头文件同步补充接口说明。

修复后真实验收已完成：

- d3 批准普通外部文件读取后，bash 的 authority=decision_model、sandbox=read_only，只追加该文件读取范围，成功返回 `project_code=orchid`。
- 真实 TUI 三轮使用 bash 依次 cat a.txt、a.txt、b.txt，审批框次数为 1、0、1；三次执行均为 read_only 且读取成功，人工批准为 exact_file_read / allow_session。
- headless 在 d3 转人工后不启动命令；敏感 `.pem` 路径不调用 d3，直接要求人工批准且不启动命令。
- 普通工作区写命令仍自动使用 workspace_write，成功写入 `p4-workspace.txt`。
- 两次源码修改后分别构建通过；最终链接成功。未新增项目测试代码、模拟模型或测试构建目标。

修复前材料：`temp/d3-acceptance/bash-outside.jsonl`、`bash-outside.err`。
修复后材料：`temp/d3-acceptance/p4-fix/` 中的 `approved-final.jsonl`、`no-approver.jsonl`、`sensitive.jsonl`、`workspace-write.jsonl`、`tui-bash-sequence.ansi`、`tui-bash-sequence.json` 和 `tui-records.json`。
其中 `approved.jsonl` 保留审批修复后暴露单文件扫描错误的中间记录，最终成功结果以 `approved-final.jsonl` 为准。

## 4. 接入过程中确认的环境与协议边界

- 临时 Home 的 runtime 最初整体链接到主 Home，导致 SRT 探测工作目录的可写范围不匹配。改为独立 runtime 状态目录、复用已准备的包路径后，sandbox status 实测成功：SRT 0.0.77、Node v24.21.0。材料：`sandbox-status.json`。
- 长临时 Home 路径与长 MCP server 名组合触发 SRT Unix socket 创建失败；使用较短的 `temp/m/` 后进入正常 MCP 握手。此项记录为部署路径限制，未修改运行库或项目代码。
- 安装的官方 `@modelcontextprotocol/server-filesystem` 1.0.0 使用旧协议，在 server/discover 上返回 -32601；DeepWiki 对本次 2026-07-28 请求返回不支持版本。它们不适用于当前只支持新协议的客户端，不能把这些连接失败计作 MCP 动作审批失败。
- [MCP 新版版本说明](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/basic/versioning.mdx)区分仅新协议客户端与跨协议客户端；DAgent 目前明确不回退 initialize。本次选择兼容服务，不改其协议范围。
- Context7 的 discover 及其他服务的原始探测响应位于 `public-mcp-discovery.json`；官方 Filesystem 的失败材料为 `mcp-read*.jsonl`、`mcp-fs-read.jsonl`、`mcp-short-home.jsonl` 及对应 stderr。

## 5. 当前剩余工作

1. 若要声明完整 MCP 传输覆盖，补充兼容新协议的真实 stdio 服务验收；当前 HTTP 查询型 MCP 已通过。
2. 人工观察 TUI 布局等视觉细节；本次已完成真实终端按键与审批行为验收。
3. C1：功能验收收尾后共同准备样本、搜索正式阈值与上下界。已有临时阈值和少量成功调用均不代表完成校准。

## 6. 提交前全面检查与修复

本轮按用户要求检查 P1–P8 的实际实现并直接修复，未新增项目测试代码或模拟服务。构建命令 `cmake --build build/dev -j 2` 通过，`git diff --check` 通过；以下材料位于 `temp/d3-acceptance/full-review/`，使用独立 Home `temp/r/`，旧配置兼容验证使用 `temp/n/`。

主要修复：

- 脚本审查：多个目标逐个审阅，任一目标失败后不能被混合命令的普通联网审阅覆盖；未修改且 host 已放行时复用规则，修改过且 HEAD diff 为空时仍复审。未配置 d3 时沿用既有联网规则。相对脚本遇到 `cd` 不再错误读取工作区根下的同名文件；取证先检查读取权限，Git 操作使用审阅截止时间和取消信号，截断证据不能用于批准。
- 决策判定：最高概率类别包含 unknown，并列不批准；fetch/send 的方向概率与相关性分别检查，send 相关性须独立标定。net_change 只使用固定界限。脚本相关性失败保留可反馈的失败维度。
- 人工反馈：普通联网和 URL 人工批准也回填审阅编号；仅明确的用户最终决定参与学习，没有人工渠道、取消、撤权不计为用户拒绝。审阅链不完整、证据不可用或缺标定时不调整阈值。
- 网页授权：每跳先生成审批身份，人工批准后保留审阅编号；会话授权更新 revision 后使用新 revision 建立该跳网络闸门，并记录实际网络目标。扫描记录关联会话、调用与执行；HTTP 头解析只跳过 1xx，避免把以 HTTP/ 开头的正文继续当响应头；审批等待计入整体网络截止时间。
- 文件授权：解析符号链接并区分目录，目录不降格为精确文件授权；普通只读命令保持 read_only。读取路径识别补齐常用选项值、grep 模式与输入重定向，避免将选项值误当文件、遗漏真实输入文件。
- 服务与并发：决策请求排队可取消且受超时限制；响应模型标识和所需概率必须有效。阈值读取失败转后续审批，审阅库不可用仍保留“已配置”语义。整轮关闭开关改为原子状态，父复批返回时重查开关及撤权状态。部分 decision_review 配置可使用默认超时，恢复的 headless unrestricted 同样输出警告。

真实回归结果：

| 场景 | 结果 | 材料 |
| --- | --- | --- |
| 外部普通文件、指向外部文件的符号链接、输入重定向、grep -e | 均经 outside_read 审阅并在 read_only 下读取成功 | `outside-read*`、`symlink-read*`、`input-read*`、`grep-option*` |
| 外部目录 | 0 次 d3；无人工渠道不执行；真实 RPC 人工批准后以 read_only 成功列出目录 | `outside-directory*`、`directory-manual*` |
| 未修改脚本与已放行 host | 成功联网，0 次 d3 | `script-allowed*` |
| 多目标脚本 | 两个目标分别产生 script_basic 审阅，命令成功 | `multiple-basic*` |
| 超过 300 行脚本混合 curl | 证据超限转审批，未被普通命令审阅覆盖，网络未获准 | `mixed-large*` |
| 修改时间晚于会话、HEAD diff 为空、host 已放行 | 仍产生 script_diff 复审；本次真实 net_change 落入灰区，无人工渠道未放行 | `script-modified-empty*` |
| 人工批准 URL、普通联网和未修改脚本 | 三种场景均保留审阅编号、写入 user / approved 反馈；唯一失败的 relevance 从 1.0 调整至 0.9 | `url-manual*`、`network-manual*`、`script-manual*` |
| 无人工渠道与真实超时 | 转审批后不误批准；没有用户反馈记录 | `headless-no-feedback*`、`decision-timeout*` |
| 扫描未标定及同轮关闭 | 不返回网页正文；后续 bash 联网不再调用 d3，未获放行 | `scan-unavailable*`、`gate-close*` |
| HTTP → HTTPS 重定向与长结果切片 | Python 官方 sqlite3 文档抓取成功；两条 fetch_url 审阅、一次 web_content 扫描；约 30 KB 交付文本抽样为四段正文，三处分隔标记，扫描 clean | `python-redirect*`、`temp/r/data/decision.sqlite` |
| ask 模式网页会话授权 | 0 次 d3；真实 RPC allow_session 后当次请求正常取得 HTTP 200，授权 revision 更新未导致误取消 | `ask-url-session*` |
| 真实 MCP | Context7 resolve-library-id、query-docs 再次经 d3 批准且成功返回 | `mcp-final*` |
| 旧 models.json 无决策字段 | 修改脚本访问已放行 host 仍成功执行，无 d3 请求 | `disabled-script-allowed.jsonl` |

GitHub 的重定向本次完成两跳 URL 审阅后遇到远端 TLS EOF；SQLite 网站本次遇到代理 502，保留失败材料，未算作成功抓取。使用 Python 官方文档完成了同一路径的完整验证。旧配置检查脚本在操作成功后查询不存在的审阅表报错，原始 JSONL 已确认工具执行成功；这属于临时结果汇总脚本，不是应用错误。

本轮使用的 review-pass / review-manual / review-noscan 都是触发功能分支的临时阈值。以上结论不替代 C1 标定，也不扩大 HTTP 查询型 MCP、真实终端行为验收的覆盖声明。
