# TUI 改版计划

设计日期：2026-10-20。状态：进行中。效果预览：[DAgent TUI 新界面设计](https://claude.ai/artifact/QVi4ezzk4oJpudJgZrESuB)（10 个画板）；OpenCode Go 登录与选模型见 [模型登录与请求头计划](model-auth-plan.md)。

## 1. 目标

1. 界面更现代：信息层次清楚、留白充足、状态一目了然。
2. 支持简单的外观自定义：主题预设、背景模式、强调色、面板样式、密度、图标、信息栏位置。
3. 新增缺少的组件：顶部标题栏、底部快捷键提示栏、设置面板、欢迎页、子 Agent 标签页、两栏模型选择、左右对比 diff 等。

## 2. 视觉总则

- 中性三层底色（页面、面板、元素）加一个强调色；层次靠亮度、间距、字重和标签块表达，圆角线框只用于浮层。
- 标签块（chip）：元素底色加一格左右留白的短标签，用于模式、模型、分支、排队消息等。
- 状态色沿用现有语义：成功与新增为蓝色、失败与删除为红色、警告为黄褐色、进行中为次强调色，色盲可区分。
- 图标默认 Unicode（`◆ ✓ ✗ ⠋ ▸ ● ○ ⎇ ▣`），可选 Nerd Font 或 ASCII。
- 界面文案仍为英文，统一来自 `ui/strings`。

## 3. 布局

| 区域 | 内容 |
| --- | --- |
| 标题栏（新增，1 行，面板底色） | 左：`◆ DAgent` / 会话标题；右：权限模式、模型、分支三个标签块 |
| 子 Agent 标签页（新增，有子 Agent 时显示） | `main` 与各 `task · agent`，带运行状态图标；Ctrl+A 切换 |
| 对话区 | 内容栏居中；用户消息为元素底色加强调色竖条，右侧显示时间 |
| 信息栏（重做） | 分节：SESSION（标题、开始时间、轮数）、CONTEXT（用量条、token、价格已知时的费用）、PLAN（进度与条目）、CHANGED（改动文件与增删行数）、AGENTS（有子 Agent 时）、MCP（状态）；底部版本号。可放左侧、右侧或隐藏 |
| 输入框 | 面板底色加竖条；尾行左侧为模式、模型标签块，右侧为 `↵ send · ⇧↵ newline`（忙时为 `↵ queue`） |
| 提示栏（替换状态栏，1 行） | 左：项目路径；右：随场景变化的按键提示（空闲、运行中、浮层打开、审批中各不相同） |
| 窄屏（信息栏让位时） | 标题栏改为图标、会话标题、模型与上下文百分比；计划在对话中原位显示 |

## 4. 组件

| 组件 | 改动 |
| --- | --- |
| HeaderBar | 新增 |
| HintBar | 新增，替换 StatusLine；上下文用量移入信息栏（窄屏时进标题栏） |
| SidePanel | 按第 3 节分节，上下文用量条 |
| Chip | 新增绘制辅助，供标题栏、输入框、排队、欢迎页使用 |
| Transcript 工具卡片 | 状态图标、加粗名称、muted 参数、右对齐统计；diff 行使用增删底色；折叠行写明剩余行数与展开键 |
| 活动行与排队 | 活动行显示耗时、token 数与生成速度；排队消息以标签块显示，`↑ recall` |
| Panel | 支持分组标题、匹配字符高亮、别名与快捷键列；命令面板使用分组 |
| 浮层背景 | 模态浮层打开时底层界面变暗 |
| ApprovalDialog | 风险类别标签、命令预览框（语法着色）、cwd/需要的权限/理由/d3 审阅结论、按键样式选项 |
| ToastStack | 按严重级别着色边框与图标 |
| WelcomeView | 新增：字标、项目/分支/模型/权限/MCP/记忆标签、最近会话（可直接恢复）、提示 |
| AgentTabs | 新增：子 Agent 标签页 |
| SettingsDialog | 新增 `/settings`：分节导航（第一版只有 Appearance），实时预览，Enter 保存，Esc 撤回 |
| ModelPicker | 两栏：左侧已配置模型与登录提供方，右侧当前项详情；OpenCode Go 登录与模型列表见 model-auth 计划 |
| 左右对比 diff | 宽屏时编辑卡片展开为左右对比，带行号；Ctrl+D 切换上下对照 |

## 5. 外观自定义

`config.json` 新增 `ui.appearance`：

| 键 | 取值 | 说明 |
| --- | --- | --- |
| `theme` | `dagent`、`midnight`、`paper` 或主题文件路径 | 预设放在 `home/themes/`，`/settings` 列出 |
| `mode` | `dark`、`light`、`auto` | `auto` 跟随终端背景（现有行为） |
| `background` | `solid`、`transparent`、`#rrggbb` | `transparent` 使用终端默认背景，让终端自己的壁纸、毛玻璃或透明度透出；自定义颜色时面板与元素底色按亮度自动推算 |
| `accent` | `#rrggbb` | 覆盖主题的强调色 |
| `panels` | `filled`、`outline` | `outline` 时面板不填底色、改画线框 |
| `density` | `comfortable`、`compact` | 紧凑模式减少块间空行与标题栏高度 |
| `icons` | `unicode`、`nerd`、`ascii` | |
| `sidebar` | `right`、`left`、`hidden` | |
| `diff` | `auto`、`unified` | `auto` 在宽屏时用左右对比 |

`/settings` 中的修改立即生效；Enter 写回 `config.json` 的 `ui.appearance`（经后端写入），Esc 恢复打开前的状态。终端不支持真彩色时，自定义颜色按现有 256 色量化规则输出。

## 6. 分期

| 期 | 内容 | 框架 |
| --- | --- | --- |
| 1 外观基础 | `ui.appearance`、主题预设、背景模式、强调色、面板样式、密度、图标、信息栏位置；`/settings`；HeaderBar、HintBar；信息栏分节与用量条；Chip | 不改框架 |
| 2 对话与浮层 | 工具卡片、活动行与排队、Panel 分组与高亮、命令面板、浮层变暗、审批框、toast、欢迎页、子 Agent 标签页 | 不改框架 |
| 3 模型与登录 | 两栏模型选择，按 [model-auth 计划](model-auth-plan.md) 实现 OpenCode Go 登录与模型列表、凭据与请求头 | 不改框架 |
| 4 宽屏增强 | 左右对比 diff；终端可点击链接（OSC 8） | 解冻 TUI 框架，按需扩展渲染与单元格属性 |

每期在 `feat/tui-redesign` 分支上完成并验收后提交，合入 `master` 由用户决定。

## 7. 不做

背景图片（kitty 图形协议）暂不做；终端自带背景图配合 `transparent` 模式可达到类似效果。界面多语言不在本计划内。

## 8. 验收

只做构建检查与真实运行，检测放在 `temp/`：

- 在真实 PTY 中运行 `dagent`，按每期内容操作，截取屏幕内容核对布局与颜色。
- 真彩色与 256 色各一次；终端宽度 80、120、160 列。
- `/settings` 修改后重启仍生效；Esc 撤回不写文件。
- `transparent` 模式下输出的背景为终端默认色。
- 不新增测试代码、模拟服务或测试目标。
