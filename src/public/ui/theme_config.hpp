// 应用层：主题文件加载（TUI 框架 §1 把主题文件的读取划给应用层，
// 框架只定义 ThemeTokens 与 epoch 失效规则，见 §4.5）。
//
// 主题文件是 JSON（默认主题见 config/themes/dagent.json）：
//   {
//     "name": "dagent",
//     "defs":  { "blue": "#7aa2f7", ... },        // 调色板，令牌按名字引用
//     "dark":  { "<令牌>": <样式>, ... },           // 暗色背景下的令牌
//     "light": { "<令牌>": <样式>, ... }            // 亮色背景下的令牌
//   }
// 样式可以是完整形式 {"fg": 颜色, "bg": 颜色, "attrs": ["bold", ...]}，也可以
// 直接写一个颜色（只设前景色）。颜色取值："#rrggbb"（真彩色，低能力终端由
// present 量化到 256 色）、0–255 的整数（256 色索引）、defs 里的名字、
// "default"（终端默认色）。属性名：bold / dim / italic / underline / blink /
// reverse / strike。令牌名与 ThemeTokens 的字段同名；文件里没写的令牌沿用
// 应用层 builtin_theme() 的取值；普通令牌的默认前景/背景在应用时继承正文/底色。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "tui/widget.hpp"

namespace dagent::ui {

struct ThemeSet {
    std::string name;
    tui::ThemeTokens dark;
    tui::ThemeTokens light;

    // 按终端背景色选明暗（与框架 default_theme 同一规则：相对亮度 > 0.5
    // 取 light，取不到背景色时取 dark）。应用在 Runtime::on_caps 回调里调用，
    // 设置前须递增返回副本的 epoch，块的物化缓存才会失效。
    const tui::ThemeTokens& pick(const std::optional<tui::Color>& background) const;
};

// 读取并解析主题文件。文件打不开、JSON 非法或颜色无法识别时抛出
// std::runtime_error / nlohmann::json 的异常。
ThemeSet load_theme(const std::filesystem::path& file);

/// 应用层完整明暗配色，包含底色与正文前景，不依赖终端默认色。
tui::ThemeTokens builtin_theme(bool light);
/// 未指定背景的文字令牌继承主题底色，保留 selection 的叠加语义。
tui::ThemeTokens resolve_theme(tui::ThemeTokens);

struct ThemeInfo {
    std::string name;
    std::filesystem::path path;
    bool available = true;
    std::optional<ThemeSet> loaded;
};

/// 列出目录中的 JSON 主题；坏文件保留为不可用条目，供选择面板灰显。
std::vector<ThemeInfo> list_themes(const std::filesystem::path& directory);

} // namespace dagent::ui
