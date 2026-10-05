# 使用指南

先完成 [构建与安装](build.md)。以下命令假定 `dagent` 在 PATH 中；开发时使用 `build/dev/src/dagent`。

## 配置与准备

Home 的选择顺序为 `DAGENT_HOME`、dev 构建的源码树 `home/`、正式构建的可执行文件目录。
Home 必须可写；前后端二进制始终放在同一目录。配置文件各司其职：

| 文件 | 用途 |
| --- | --- |
| `config/config.json` | 权限、工具预算、网页引擎与缓存、上下文、超时、UI 和日志 |
| `config/models.json` | 模型配置名、协议、地址、模型 ID、密钥及默认模型 |
| `config/mcp.json` | MCP 连接、环境与显式权限 profile |
| `config/runtime.json` | 托管工具、依赖版本和锁文件 |
| `agents/*.md` | 子 Agent 的工具名单、权限上限、模型和预算 |

安装会把 `models.example.json`、`mcp.example.json` 转为缺失的配置文件；开发源码树需自行从示例准备。
模型和 MCP 配置要求权限为 `0600`。不要把真实密钥放进示例文件。
配置中的相对路径通常相对于 Home；沙箱额外路径相对于工作目录。详见 [配置契约](../design/app.md#2-配置与提示词)。

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
