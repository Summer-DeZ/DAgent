# 04 运行时切换

> 里程碑 P4 · `src/private/ui/shell.cpp`、`src/public/agent/agent.hpp`

`/model` 选一套配置，当前会话接着用新模型跑。

---

## 1. 复用已有的换 Agent 路径

核心是「一个会话一个模型」，但 `Agent::resume` 本来就允许用另一个模型恢复同一个会话——
`agent.cpp` 里已经有「This session used X; continuing with Y」这条 Notice。所以切换不用新机制：

```
用户选中 → 工作线程：Agent::resume(当前会话 id, 新的 ProviderConfig)
        → 锁内换指针、锁外析构旧 Agent（与 /sessions 同一条路径）
        → post 回渲染线程：不清空对话，只更新模型名与上下文上限
```

会话 id 不变、历史不丢、记录里自然留下一条模型变更。**不新建会话**——新建会话是 `/new` 的语义。

界面上要同步的：侧栏的 Context 上限、输入框尾行的模型名、消息尾行的模型名
（`transcript_.set_session(mode, model)` 与 `update_prompt_footer()` 都已存在）。

## 2. 面板

`/model` 与 `ctrl+m` 打开通用 `Panel`，行来自配置的 `models` 表：

```text
╭─ Model ─────────────────────────────────────────────────────────╮
│ Search: █                                                       │
│                                                                 │
│▌local        openai-chat   Qwen3.8-Flash-Next        current    │
│ deepseek     openai-chat   deepseek-chat                        │
│ sonnet       anthropic     claude-sonnet-5                      │
╰──────────── enter switch · esc close ───────────────────────────╯
```

- 三列：名字、kind、model；右列标 `current`。
- 切换期间面板关闭、活动行显示 `switching model …`，这段时间的输入进排队。
- 失败（密钥缺失、resume 抛错）弹 error toast，保留当前 Agent。
- 忙碌时禁用该命令（`enabled` 返回 `!busy_`），不在一轮跑到一半换模型。

## 3. 换模型时必须跟着变的东西

| 东西 | 做法 |
| --- | --- |
| 上下文上限 | 新 `ProviderConfig::context_window`（为 0 回落全局）；下一次 `ContextUpdate` 自然带新 limit |
| 已超出新窗口的历史 | 切换后第一轮照常走自动压缩，不做额外特判 |
| 历史里的 `reasoning_content` | 默认不回传（`send_reasoning_content=false`），跨厂商最安全 |
| 工具调用 id | 内部格式保存的是上一家的 id，只在回填时用；新一轮的调用由新 provider 生成 |
| 系统提示词 | 不变（同一个会话同一份提示词），`resume` 已经会重写 system 记录 |

**已知边界**：上一轮结束在「assistant 有 tool_calls、tool 结果还没回填」的状态时不允许切换——
Conversation 的不变式要求同一轮内配对。实现上靠「忙碌时禁用」自然避免；恢复到空闲时一定是配对完整的。

## 4. 完成标准

1. 本地模型与远端模型之间来回切三次，会话 id 不变、历史完整、能接着问上下文相关的问题。
2. 切换后让它调一次工具，调用与回填正常。
3. 侧栏 Context 上限、输入框尾行、消息尾行三处的模型名都跟着变。
4. 把某个条目的密钥环境变量去掉再切：error toast，当前会话不受影响。
5. 切过模型的会话 `dagent -r` 回放正常。
6. 忙碌时 `/model` 不可用（命令面板里灰显）。
