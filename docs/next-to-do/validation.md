# Provider 实施与真实验收记录

日期：2026-09-20。仅使用构建、真实 dagent CLI/PTY 和真实模型服务；没有添加测试代码、模拟服务或构建目标。
检测配置、会话、终端输出与 HTTP 响应均保存在 `temp/providers/`（临时产物不提交）。

## 状态

| 里程碑 | 实施 | 验收 |
| --- | --- | --- |
| P1 内部格式 | 已完成 | 新版可恢复改造前会话；非空 signature 的远端往返尚无条件验证 |
| P2 Provider 抽象 | 已完成 | 本地 Qwen 工具回合与基线一致；CLI 和 PTY 均通过 |
| P3 多模型配置 | 已完成 | 选择/覆盖顺序、列表、分层合并、错误定位均真实运行通过 |
| P4 运行时切换 | 已完成 | Qwen ↔ Ollama、保留历史、工具、错误保留、忙碌禁用和回放通过；远端待 key |
| P5 Ollama | 已完成 | 原生 NDJSON、工具读写、显式窗口和长输入通过 |
| P5 Anthropic | 代码与构建完成 | 用户确认暂时没有 key；未进行真实验收，不合并或宣称已验证支持 |
| P6 文档 | 已完成 | design/llm、app、ui、agent 同步，已验证与待验证明确区分 |

## 构建

`cmake --build build --target dagent -j 4` 成功，最后输出为 `[100%] Built target dagent`。
记录：`temp/providers/build.log`。只构建应用目标，没有运行或增加测试目标。

## 本地模型与工具

- 改前基线：Qwen3.8-Flash-Next 读取规划 README 前 8 行，再回答标题；2 步、1 次工具、status=done。
  会话 `01a0be05-5cfb-705d-9d1a-0ba1fb49bfb0`，记录 `baseline.json`。
- 改后相同请求：同一模型仍为 2 步、1 次工具、标题相同、status=done，流含 usage 与 stop/tool_calls。
  会话 `01a0be0b-c207-7805-a764-c970cfb92496`，记录 `qwen.json`。
- 使用新程序恢复基线会话（原记录没有 reasoning_signature），不调用工具仍能回答原文档标题。
  记录 `old-record-resume.json`。
- Ollama 服务原先无模型，按规划拉取 qwen3:1.7b。原生协议完成 read 与 edit，真实文件
  `temp/providers/note.txt` 从 `Status: planned.` 改为 `Status: ready.`，返回项目代号 Amber Lantern。
  会话 `01a0be0b-c231-7fcb-a6d0-8211be0dc602`，2 步、2 次工具、status=done，记录 `ollama.json`。
- 模型窗口设 0、全局设 16384 的长文档请求：实际 prompt=12788、completion=11，找回开头标记 MAGNOLIA-7834。
  记录 `ollama-long-document.json`；服务端 `/api/ps` 报告 context_length=16384。
- `extra_body.max_completion_tokens=128`：真实 Qwen 请求返回 OK，ContextUpdate.limit=253824，
  等于 262144−8192−128，记录 `completion-tokens.jsonl`。

## 配置

- 三套本地配置 local、local-small、ollama：名称选择、未命中名称时覆盖模型 ID、与 --set 混用顺序正确。
- 独立临时项目与 XDG 用户配置真实合并：项目只覆盖 local.model，其 base_url 和用户级 second 条目保留。
- 未知 kind、空 model、缺密钥变量、缺必需 max_tokens、不存在的默认项、空 models、旧 gateway、负窗口：
  均以退出码 2 拒绝，错误指出对应字段。现有三个 kind 均有默认端点，空端点按注册表回落。
- `--list-models` 不要求终端，密钥只显示 yes/no；本次可用条目均无 key。
- 初始化日志后能看到 named configuration 与 overrides model id 两条不同说明。

记录：`config-results.txt`、`merge-results.txt`、`list-models.txt`、`selection.log`。

## 真实交互与恢复

会话 `01a0be0d-773f-7d1d-b266-c5287a9e1619`：

1. 本地 Qwen 记住 AMBER-527。
2. Ctrl+M 切到 Ollama，正确复述 AMBER-527。
3. 切到 local-small，用 read 读文件，同时返回文件代号与记忆中的 AMBER-527。
4. 再切到 Ollama，用 read 读同一文件并回答代号。
5. 临时将目标配置指向不存在的密钥变量，切换弹出 Cannot switch model，system 记录数不变。
6. 同一 Agent 继续正确回答 AMBER-527；修复临时配置后用 `/model` 切回 local。
7. `-r` 恢复同一 ID，历史文本、各轮模型标签和工具卡片保留；正常退出码 0。
8. 单独真实回合中忙碌时提交 `/model` 被拒绝并提示 unavailable；Ctrl+M 不打开切换面板。

输入框和消息尾行显示实际模型；上下文预算随配置切换，模型变化写入 system 记录。
PTY 原始输出：`tui.raw`、`tui-resume.raw`、`tui-busy.raw`；摘要：`tui-results.txt`、`tui-resume-results.txt`。
第一轮终端自动操作等待完整密钥变量名超时（toast 按宽度截断）；恢复会话后按可见错误提示完成了验收，
不是一次产品切换失败被忽略。

## 错误与剩余边界

- 真实 Qwen 超长请求返回 HTTP 400：270052 tokens 超出 262144，原文含 available context size。
  dagent 运行进入上下文超长处理；仅有一个巨大用户消息，没有可压缩历史，最终提示新建会话。
  记录 `qwen-overflow-http.json`、`qwen-overflow.json`。
- Ollama 原生真实短输出上限返回 done_reason=length，记录 `ollama-length.ndjson`。
- 重复词填充的一次长输入出现正文后流中无结束事件，客户端重试后明确失败，未误报成功。
  此异常尚未归因；随后真实文档长输入成功，记录分别为 `ollama-long.json` 与 `ollama-long-document.json`。
- Anthropic 没有可用密钥：真实读/改文件、缓存 usage、工具分片、signature、400/429/529 均未验收。
- DeepSeek/智谱及其它远端兼容服务未验收；本次没有制造真实 429，retry-after 未经真实限流验证。
- Ollama 超出已配置窗口的服务端错误分类未验证；已验证的是显式 num_ctx 和窗口内长输入，
  不声称 Ollama 在任何超限情况下都会报错。
- Anthropic extended thinking 明确未开放。需要完整 thinking 块保真回传时另行规划。
