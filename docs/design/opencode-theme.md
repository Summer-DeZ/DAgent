# DAgent 配色与主题文件

DAgent 早期配色参考本机 OpenCode `orng`；phase 5a 改为
[改版计划 §9.3](../archive/2026-10-20-tui-redesign-plan.md#93-配色) 批准的配色。
只保留 `home/themes/dagent.json`，框架 dark_theme / light_theme 同步提供完整缺省值。
`midnight.json`、`paper.json` 已删除；`ui.appearance.theme` 接受 `dagent` 或主题文件路径，
旧预设名称报配置错误，提示使用 dagent 或文件路径。`/settings` 删除 Theme 行，`/theme` 仍定位 Mode。

## 配色

| 令牌 | dark | light |
| --- | --- | --- |
| background / panel / element / selected | `#121212` / `#1c1c1c` / `#262626` / `#303030` | `#ffffff` / `#eeeeee` / `#e4e4e4` / `#dadada` |
| border / border_active | `#3a3a3a` / `#585858` | `#c6c6c6` / `#9e9e9e` |
| text / text_muted / text_faint | `#e4e4e4` / `#b2b2b2` / `#8a8a8a` | `#1c1c1c` / `#4e4e4e` / `#6c6c6c` |
| primary（orange） | `#fe8a4c` | `#a85500` |
| success / error / warning / info | `#6eb6ff` / `#f24e6f` / `#f1ce59` / `#5ad7d7` | `#0261ca` / `#9b0655` / `#4c4300` / `#007179` |
| markdown_code | `#d8b0ff` | `#8b3bab` |
| diff_added / diff_removed | `#abcdff` / `#f68486` | `#005c94` / `#860f23` |
| diff_added_bg / diff_removed_bg | `#21344f` / `#381617` | `#e1f1ff` / `#facecd` |
| syntax_keyword / string / number / function / type | `#c592e7` / `#a6d7a0` / `#f7a97c` / `#81a9fd` / `#81dbcb` | `#8639c7` / `#297d0f` / `#9a5505` / `#215fbc` / `#107a6c` |

accent 为 primary 向 text 混合 20%；selection 为页面色文字、primary 底色。
error 不加粗，markdown_heading 为 text 加粗，markdown_link 为 info 加下划线，
syntax_comment 为 text_faint 加斜体；diff_context 为 text、diff_hunk 为 text_faint，markdown_quote 为 text_muted 加斜体。
三档文字的用途见 [UI 设计](ui.md#2-外观与设置)，text_faint 只用在页面/面板底色上。

## JSON 格式

顶层保留 `name`、`defs`、`dark`、`light`。defs 为颜色别名表；dark/light 的键与 ThemeTokens 字段同名，
未提供的令牌沿用内置对应明暗主题。新增 text_faint、background_selected、diff_added_bg、diff_removed_bg。
完整字段表见 [框架主题](tui-framework.md#45-控件与主题l4)。

样式可为一个颜色（只设置 fg），或 `{"fg": 颜色, "bg": 颜色, "attrs": [属性名]}`。
背景令牌的颜色写在 bg。属性包括 bold、dim、italic、underline、blink、reverse、strike。
普通令牌的默认 fg/bg 在应用时继承正文/页面，浮层继承面板底色；selection 保留独立样式。

颜色接受：

- `"#rrggbb"`：RGB；无真彩色时自动量化。
- `0`–`255` 整数：始终使用 xterm 索引。
- `"default"`：终端默认色。
- defs 中的名字：引用对应颜色，别名值也可以使用下面的对象形式。
- `{"rgb": "#21344f", "256": 17}`：真彩色使用 RGB；仅在无真彩色时使用显式索引，索引范围 0–255。

例如（部分主题文件）：

```json
{
  "name": "custom",
  "defs": { "added": { "rgb": "#21344f", "256": 17 } },
  "dark": {
    "text_faint": "#8a8a8a",
    "background_selected": { "bg": "#303030" },
    "diff_added_bg": { "bg": "added" },
    "diff_removed_bg": { "bg": { "rgb": "#381617", "256": 52 } }
  },
  "light": {
    "diff_added_bg": { "bg": { "rgb": "#e1f1ff", "256": 195 } },
    "diff_removed_bg": { "bg": { "rgb": "#facecd", "256": 224 } }
  }
}
```

未带显式索引时，出帧器比较 xterm 色立方 `0,95,135,175,215,255` 和灰阶的 RGB 距离。
中性色对齐 xterm 灰阶：暗色四层为 233/234/235/236，亮色为 231/255/254/253。
solid diff 使用上面的固定底色与索引；transparent / 自定义背景仍动态混合 success/error 的 15%，
分别以面板/当前页面为基底，生成的 RGB 使用普通量化，不保留 solid 的固定索引。

## 强调色与验证范围

`ui.appearance.accent` 接受 orange（默认）、green、violet、blue 或自定义 hex。
预设按名称保存并随明暗解析，值及旧 hex 迁移见 [应用配置](app.md)。
自定义色在 OKLab 中保持色相、调整明度，在浅色压暗、深色提亮到面板上 4.5:1；
出色域时降低色度，保存值保持用户输入。secondary accent 继续由最终 primary 混合得到。

phase 5a 只执行指定 CMake 构建。真彩/256 色、深浅色、透明/自定义背景的实际 PTY 颜色与对比度由后续运行验收。
