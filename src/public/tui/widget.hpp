/// @file widget.hpp
/// @brief 视图层：具体视图控件。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tui/layout.hpp"

namespace dagent::tui {

/// @brief 语义主题令牌：控件与渲染器只引用令牌，不写死颜色。
struct ThemeTokens {
    Style text, text_muted, primary, accent;
    Style border, border_active, background, background_panel,
        background_element;
    Style success, warning, error, info;
    Style diff_added, diff_removed, diff_context, diff_hunk;
    Style syntax_keyword, syntax_string, syntax_comment, syntax_number,
        syntax_function, syntax_type;
    Style markdown_heading, markdown_code, markdown_link, markdown_quote;
    Style selection;
    uint32_t epoch = 0; ///< 样式变更后递增（使 Document 缓存失效）
};

/// @brief 内置 dark / light 主题令牌。
const ThemeTokens& dark_theme() noexcept;
const ThemeTokens& light_theme() noexcept;

/// @brief sRGB 相对亮度（0..1）；非 RGB 返回 0。
float relative_luminance(const Color& c) noexcept;

/// @brief 按背景亮度选择 dark / light；无背景信息时返回 dark。
const ThemeTokens& default_theme(const std::optional<Color>& background) noexcept;

/// @brief 多行静态文本；行不折行，越界裁剪。
class Text : public Widget {
public:
    /// @brief 重置内容（行数与自然宽度可能变化，触发重新布局）。
    void set_text(std::string s);
    [[nodiscard]] const std::string& text() const noexcept { return text_; }

    /// @brief 设置样式并重画。
    void set_style(const Style& s) noexcept {
        style_ = s;
        invalidate();
    }
    /// @brief 设置主题（底纹取 background 令牌）。
    void set_theme(const ThemeTokens& t) noexcept {
        theme_ = &t;
        invalidate();
    }

    Size measure(Size available) const override;
    void render(Surface& s) override;

private:
    void resplit(); ///< 重算行切片与自然宽度

    std::string text_;
    std::vector<std::string_view> lines_; ///< text_ 的行切片
    Style style_{};
    int width_ = 0; ///< 自然宽度（最长行的显示宽度）
    const ThemeTokens* theme_ = &dark_theme();
};

/// @brief 动作行：转圈符号 + 动作文本；空闲时高度 0。
class Activity : public Widget {
public:
    /// @brief 设置动作；空文本 = 空闲。
    void set_action(std::string text);
    /// @brief 推进转圈帧。
    void tick() noexcept;

    /// @brief 设置主题（只保存引用，主题对象须比控件活得久）。
    void set_theme(const ThemeTokens& t) noexcept {
        theme_ = &t;
        invalidate();
    }

    Size measure(Size available) const override;
    void render(Surface& s) override;

private:
    std::string action_;
    int width_ = 0;     ///< 自然宽度（符号 + 空格 + 文本）
    uint8_t frame_ = 0; ///< 转圈帧索引
    const ThemeTokens* theme_ = &dark_theme();
};

/// @brief 单行通知提示条；空文本时高度 0。
class Notice : public Widget {
public:
    enum class Severity { info, warn, error };

    /// @brief 显示通知；空文本 = 清除。
    void show(Severity sev, std::string text);
    /// @brief 设置主题（只保存引用，主题对象须比控件活得久）。
    void set_theme(const ThemeTokens& t) noexcept {
        theme_ = &t;
        invalidate();
    }

    Size measure(Size available) const override;
    void render(Surface& s) override;

private:
    [[nodiscard]] Style style_of(Severity sev) const noexcept;

    std::string text_;
    Severity sev_ = Severity::info;
    int width_ = 0; ///< 自然宽度
    const ThemeTokens* theme_ = &dark_theme();
};

/// @brief 多行输入框：边框 + 文本 + 光标，支持双向滚动。
class InputBox : public Widget {
public:
    InputBox() {
        lines_.emplace_back(); // 至少一行：光标永远有落点
        line_widths_.push_back(0);
    }

    // ---- 内容模型（编辑后触发重新布局） ----

    /// @brief 整体重置，光标归位；\r\n/\r 规范化为 \n，控制符丢弃。
    void set_text(std::string s);
    /// @brief 以 '\n' 连接的全部内容。
    [[nodiscard]] std::string text() const;
    /// @brief 光标处插入文本；'\n' 分行。
    void insert(std::string_view utf8);
    /// @brief 删除光标前一个字素簇（行首则并入上一行）。
    void backspace();
    /// @brief 删除光标后一个字素簇（行尾则并入下一行）。
    void del();
    /// @brief 相对移动：水平跨行、垂直保持显示列，越界夹取。
    void move(int dcols, int dlines);
    /// @brief 移到本行行首。
    void line_home();
    /// @brief 移到本行行尾。
    void line_end();

    /// @brief 设置主题（边框取 border 令牌）。
    void set_theme(const ThemeTokens& t) noexcept {
        theme_ = &t;
        invalidate();
    }

    std::optional<Point> cursor() const override;
    Size measure(Size available) const override;
    void render(Surface& s) override;

private:
    [[nodiscard]] int caret_col() const noexcept; ///< 光标的显示列
    void caret_moved() noexcept { goal_ = -1; }   ///< 目标列失效

    std::vector<std::string> lines_;
    std::vector<int> line_widths_; ///< 每行显示宽度缓存
    int line_ = 0;    ///< 光标行
    int col_ = 0;     ///< 光标字节偏移
    int goal_ = -1;   ///< 垂直移动的显示列目标（-1 = 待定）
    int vscroll_ = 0; ///< 顶部可见逻辑行（render 维护）
    int hscroll_ = 0; ///< 左起显示列（render 维护）
    const ThemeTokens* theme_ = &dark_theme();
};

} // namespace dagent::tui
