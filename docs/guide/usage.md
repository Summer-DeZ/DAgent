# 使用指南

先完成 [构建与安装](build.md)。以下命令假定 `dagent` 在 PATH 中；开发时使用 `build/dev/src/dagent`。

## 配置与准备

Home 的选择顺序为 `DAGENT_HOME`、dev 构建的源码树 `home/`、正式构建的可执行文件目录。
Home 必须可写；前后端二进制始终放在同一目录。配置文件各司其职：

| 文件 | 用途 |
| --- | --- |
| `config/config.json` | 权限、工具预算、网页引擎与缓存、上下文、超时、UI 和日志 |
| `config/models.json` | 模型配置名、协议、地址、模型 ID、密钥、自定义请求头及默认模型 |
| `config/mcp.json` | MCP 连接、环境与显式权限 profile |
| `config/runtime.json` | 托管工具、依赖版本和锁文件 |
| `agents/*.md` | 子 Agent 的工具名单、权限上限、模型和预算 |

安装会把 `models.example.json`、`mcp.example.json` 转为缺失的配置文件；开发源码树需自行从示例准备。
模型和 MCP 配置要求权限为 `0600`。不要把真实密钥放进示例文件。
配置中的相对路径通常相对于 Home；沙箱额外路径相对于工作目录。详见 [配置契约](../design/app.md#2-配置与提示词)。

通过 CLI 登录 OpenCode Go：

```bash
dagent login opencode     # 显示授权链接与用户码，尝试打开浏览器，等待批准
dagent login              # 仅本地状态，不联网、不刷新
dagent logout opencode    # 删除本地凭据，保留锁文件
```

在浏览器选择工作区并批准；需要 Go 订阅时使用命令打印的订阅链接。无法自动打开浏览器时手工打开链接并输入用户码。
等待时 Ctrl+C 取消，成功后显示工作区和到期时间。凭据存在 Home/data/credentials/（目录 0700，JSON 0600）。
不要复制或重用旧 refresh token；遇到 needs sign-in（含不确定的刷新结果）重新运行登录命令。
本阶段尚未增加 TUI 登录框和 Go 选模型列表，CLI 登录后在 models.json 的 models 中添加条目，例如：

```json
"opencode-go/deepseek-v4.1-flash": {
  "kind": "openai-chat",
  "base_url": "https://opencode.ai/inference/go/openai/v1",
  "model": "deepseek-v4.1-flash",
  "login": "opencode",
  "context_window": 1000000,
  "max_tokens": 32768,
  "send_reasoning_content": true
}
```

用 `--model opencode-go/deepseek-v4.1-flash` 选择；实际可用模型与限制以服务清单为准，推理标记仍需真实验证。
`login` 与 `api_key` 不能同时出现，即使其中一个是 null 或空字符串也报错。
OpenCode 自动添加会话头，不必手工复制令牌或配置认证头。未登录的条目不会影响其它模型或登录命令。
`--list-models` 的 AUTH 列只显示 key、login:opencode 或空值。

每个生成模型或 decision_models 条目都可配置字符串映射 `headers`，例如 `"headers":{"x-routing-token":"${ROUTING_TOKEN}"}`。
`${ENV}` 在加载时展开，未设置或为空时报错；`{session_id}` 在每次请求时替换，缺少会话身份时报错。
主回合和压缩用所属会话，父审阅用父会话，子 Agent 用自己的会话，d3 用被审阅动作所属会话；其它花括号不变。
合并顺序是协议内置头、登录提供方默认头、条目 headers，按 ASCII 大小写无关覆盖；同一对象中的大小写重复键报错。
头名必须是 HTTP token，值不允许 CR/LF/NUL 等控制字符，不支持删除或多值头。

不能配置 Host、Content-Length、Transfer-Encoding、Connection、TE、Trailer、Upgrade、Expect、
Proxy-Authorization、Proxy-Authenticate、Content-Type、Accept、User-Agent；Anthropic 还保留 anthropic-version。
有 api_key 或 login 时禁止覆盖 Authorization/x-api-key；无托管认证的协议可自行配置认证头。
头值不公开或保存到会话记录，错误输出在截断前脱敏。decision_models 支持 headers，但不支持 login。
修改配置后重启后端生效。

```bash
dagent --list-models
dagent runtime sync
dagent runtime list
dagent sandbox status
```

`runtime sync` 准备 Bash、Git、Python、Node、rg、SRT、SearXNG 和配置声明的依赖，不下载或启动 LLM。
SearXNG 只在第一次搜索时启动，随当前后端退出；首次使用和升级准备见 [网页指南](web.md)。
确认所选模型的服务实际可访问；普通启动不会安装缺失工具或启动模型服务。
`runtime list` 的 `current=true` 仅表示环境与配置匹配，沙箱能力另看 `sandbox status` 的 `probe.ok`。

## 运行

```bash
dagent -C /absolute/project
dagent run -C /absolute/project --model local --output json "分析当前项目结构"
dagent run --read-only "查找会话恢复的入口"
dagent --plan "规划一次模块调整"
```

`--model` 优先按配置名选择条目；未命中时作为当前模型的临时 ID 覆写。
`--set key=value` 只影响本次启动，不写回配置。`run` 可从 stdin 追加提示词，
`--output` 支持 `text`、`json`、`jsonl`；交互运行要求 stdin 是终端。

交互界面可用 `/model` 切模型、`/permissions` 查看或撤销会话授权、`/skills` 查看技能、
`/compact` 压缩上下文、`/new` 创建会话。完整键位与页面行为见 [UI](../design/ui.md)。
需要人工批准的任务应使用交互界面，`run` 没有人工审批入口。权限细节见 [权限与沙箱](permissions.md)。

## 自定义外观

输入 `/settings` 打开外观设置，`/theme` 直接定位 Mode。上下选择、左右切换，立即预览全界面。
Enter 保存到当前 Home 的 `config/config.json`，Esc 或 Ctrl+C 撤回；保存失败会显示错误并保留设置窗口。
背景选 custom 或强调色选 custom 后，Enter 编辑 `#rrggbb`，Enter 应用颜色，再次 Enter 保存。

内置 dagent（中性橙）、midnight（冷紫）、paper（暖纸色），均支持 auto、dark、light。
auto 跟随终端 OSC 11 背景通知；不发送通知的终端可以手动选择明暗。
可选 filled/outline 面板、comfortable/compact 密度、unicode/nerd/ascii 图标，以及左/右/隐藏侧栏。
Nerd 图标需要终端字体包含 Nerd Font 字符。Ctrl+T 临时切换侧栏，不改已保存的配置。

透明模式的用法：先在终端中设置背景图片或透明度，再将 Background 设为 transparent。
页面使用终端默认底色，面板仍保留主题底色；选择 outline 可让栏和输入框也透出终端背景。
DAgent 本身不加载背景图片。真彩终端显示精确颜色，256 色终端会自动量化。

也可以编辑配置，例如：

```json
"ui": {
  "theme_file": "themes/dagent.json",
  "completion_max_files": 5000,
  "appearance": {
    "theme": "midnight",
    "mode": "auto",
    "background": "transparent",
    "accent": "#6ba1e6",
    "panels": "filled",
    "density": "compact",
    "icons": "unicode",
    "sidebar": "left"
  }
}
```

每个 appearance 字段均可省略；不写 appearance.theme 时继续使用旧 ui.theme_file。
自定义主题文件路径相对 Home，格式见 [UI 设计](../design/ui.md#2-外观与设置)。
直接编辑文件后重启生效；通过设置对话框保存无需重启。

## 网页调研

`web_search` 和 `web_fetch` 是模型可调用的内置工具，支持搜索、静态页面阅读与会话内分页缓存。
在 ask/workspace 下，新网页目标走网络审批；plan/read_only 也可调用，SRT 不可用时拒绝。
使用示例、headless 预授权、引擎配置和错误处理见 [网页搜索与抓取](web.md)。

## 会话与状态

```bash
dagent sessions
dagent --continue
dagent --resume SESSION_ID
```

列表和恢复按规范化后的精确工作目录归属，不把同一 Git 仓库的不同子目录合并。
`--continue` 选择当前目录最近更新的顶层会话；`--resume` 接受完整 ID 或唯一前缀。
子会话由父会话浏览，不进入上述顶层列表。

恢复重建对话，不重跑历史工具，也不恢复临时授权或网页缓存。历史中的 web 卡片保留当时结果；继续分页需重新获取页面。未正常结束的工具调用会被标记为结果未知，
不能据此推断操作没有发生。运行结果为 `limit` 表示预算耗尽，`interrupted` 表示取消，
`denied` 表示用户直接拒绝，`failed` 表示运行失败；只有 `done` 是正常结束。
父任务的 `done` 不代表每个子任务都成功，应同时查看子任务结果中的状态和 `is_error`。

配置和资源变更通常需要退出并重新启动后端。升级不清空 `data/dagent.db`；
备份活动数据库需使用 SQLite 一致备份，不能只复制主文件而忽略 WAL。详见 [存储](../design/storage.md)。
