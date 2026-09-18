// L4 视图层：从 L3 的 Widget 基类派生的具体视图（设计文档 §9）。
// 层边界：
//   * Widget 基类已并入 layout.hpp（L3），本头只放具体视图；
//   * 事件解码与焦点路由是 L6 的职责 —— 本层只提供程序化的内容与
//     编辑模型，"按键 → 模型调用"的翻译由上层完成；
//   * 不做控件库（按钮/表格/下拉/滚动条皮肤，§15）；
//   * 大内容滚动区（Scrollback）是 L5，不在本层。
//
// 渲染契约：render() 必须自绘整个视图 —— 先清底再画内容。back_ 每帧
// 从 front_ 复制而来（§5.3），失效控件的那片区域可能留着上一帧的内容。
// 视图坐标系从 (0,0) 开始，越界由 Surface 视图自动裁剪。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tui/layout.hpp"

namespace dagent::tui {

// 语义主题令牌（§9.2）：渲染器与控件只引用令牌，不写死颜色。令牌放在
// L4 是因为控件与 L5 渲染器都要用；任何样式字段变更后必须递增 epoch，
// Document 的物化缓存（cache_key 含 epoch）据此失效并整块重排。
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
    uint32_t epoch = 0;
};

// 内置 dark / light 两套令牌（256 色，present 负责按能力量化到真彩/256）。
// §6.2 取得终端背景色后按相对亮度（阈值 0.5）选择；取不到时默认 dark。
// 主题文件的格式与加载属于应用层，框架只提供令牌与选择规则。
const ThemeTokens& dark_theme() noexcept;
const ThemeTokens& light_theme() noexcept;
// sRGB 相对亮度（0..1，线性无关的加权和）；非 RGB（索引/默认）返回 0。
float relative_luminance(const Color& c) noexcept;
const ThemeTokens& default_theme(const std::optional<Color>& background) noexcept;

// 多行静态文本。逻辑内容是 '\n' 分隔的行；行不折行，超出视图宽度的
// 部分由视图裁剪（折行属于 L5 滚动区的职责）。制表符按 tab stop 展开，
// 自然宽度与渲染共用同一套宽度规则。
class Text : public Widget {
public:
    // 整体重置内容。行数与自然宽度可能变化 → 布局纪元。
    void set_text(std::string s);
    [[nodiscard]] const std::string& text() const noexcept { return text_; }

    // 样式不影响尺寸 → 只需重画。
    void set_style(const Style& s) noexcept {
        style_ = s;
        invalidate();
    }
    // 底纹取自语义令牌 background（§9.2）；内容样式仍由 set_style 指定。
    void set_theme(const ThemeTokens& t) noexcept {
        theme_ = &t;
        invalidate();
    }

    Size measure(Size available) const override;
    void render(Surface& s) override;

private:
    void resplit(); // 行切片 + 自然宽度缓存，text_ 变化后调用

    std::string text_;
    std::vector<std::string_view> lines_; // 指向 text_ 的行切片
    Style style_{};
    int width_ = 0; // 缓存的自然宽度（最长行的显示宽度）
    const ThemeTokens* theme_ = &dark_theme();
};

// 动作行：转圈符号 + 当前动作（§9.1 Activity，content(0..1)）。
// 空闲（动作为空）时自然高度 0、不占布局。tick() 只推进动画帧并置脏，
// 帧节奏由应用用 L7 的 every 定时器驱动（§12.4），本层不做任何计时。
class Activity : public Widget {
public:
    void set_action(std::string text); // 空文本 = 空闲（0 行）
    void tick() noexcept;              // 推进转圈帧

    // 语义令牌（§9.2）：只保存引用，主题对象必须比控件活得久
    // （内置 dark/light 是进程级静态，可安全引用）。
    void set_theme(const ThemeTokens& t) noexcept {
        theme_ = &t;
        invalidate();
    }

    Size measure(Size available) const override;
    void render(Surface& s) override;

private:
    std::string action_;
    int width_ = 0;     // 缓存的自然宽度（符号 + 空格 + 文本）
    uint8_t frame_ = 0; // 转圈帧索引
    const ThemeTokens* theme_ = &dark_theme();
};

// 提示条：单行通知（§9.1 Notice，content(0..1)）。文本为空时
// 自然高度 0。严重级别映射语义令牌 info / warning / error。
class Notice : public Widget {
public:
    enum class Severity { info, warn, error };

    void show(Severity sev, std::string text); // 空文本 = 清除（0 行）
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
    int width_ = 0; // 缓存的自然宽度
    const ThemeTokens* theme_ = &dark_theme();
};

// 多行输入框：边框 + 逻辑多行文本 + 光标（§9.1 InputBox，
// content(3..N)）。编辑模型是纯内存操作（O(编辑量)，零 I/O），
// L6 的按键路由只负责把事件翻译成这些调用。
//
// 光标按 (行, 字节偏移) 存放，所有编辑都在字素簇边界上进行，
// 不撕裂 UTF-8 序列。垂直移动保持显示列（目标列语义：跨行移动时
// 停在目标显示列，水平移动后重置）；超出行/列时垂直、水平双向滚动，
// 滚动只发生在 render（渲染线程）里，模型与滚动解耦。
// 光标落点由 cursor() 给出，帧末由渲染器定位（§5.3）。
class InputBox : public Widget {
public:
    InputBox() {
        lines_.emplace_back(); // 至少一行：光标永远有落点
        line_widths_.push_back(0);
    }

    // ---- 内容模型（L6 及上层驱动；编辑后都是布局纪元） ----
    // set_text/insert 入口规范化：\r\n 与 \r 转为 \n，除 \t 外的控制符丢弃。
    void set_text(std::string s);        // 整体重置，光标归位
    [[nodiscard]] std::string text() const; // 以 '\n' 连接的全部内容
    void insert(std::string_view utf8);  // 光标处插入；'\n' 分行
    void backspace();                    // 删除光标前一个字素簇（行首则并入上一行）
    void del();                          // 删除光标后一个字素簇（行尾则并入下一行）
    void move(int dcols, int dlines);    // 相对移动：水平跨行、垂直保持显示列，越界夹取
    void line_home();                    // 移到本行行首
    void line_end();                     // 移到本行行尾

    // 边框取自语义令牌 border（§9.2）。
    void set_theme(const ThemeTokens& t) noexcept {
        theme_ = &t;
        invalidate();
    }

    bool focusable() const override { return true; }
    std::optional<Point> cursor() const override;
    Size measure(Size available) const override;
    void render(Surface& s) override;

private:
    [[nodiscard]] int caret_col() const noexcept; // 光标的显示列
    void caret_moved() noexcept { goal_ = -1; }   // 目标列失效，垂直移动时重算

    std::vector<std::string> lines_;
    std::vector<int> line_widths_; // 与 lines_ 平行：每行显示宽度缓存
    int line_ = 0;    // 光标行
    int col_ = 0;     // 光标字节偏移（0..lines_[line_].size()）
    int goal_ = -1;   // 垂直移动的显示列目标（-1 = 待定）
    int vscroll_ = 0; // render 维护：顶部可见逻辑行
    int hscroll_ = 0; // render 维护：左起显示列
    const ThemeTokens* theme_ = &dark_theme();
};

} // namespace dagent::tui
