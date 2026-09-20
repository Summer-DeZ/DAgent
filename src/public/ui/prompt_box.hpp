#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "tui/widget.hpp"
#include "ui/strings.hpp"

namespace dagent::ui {

/// 输入框：左侧竖条 + 底纹，内容自动折行并按折行后的行数增高，
/// 框内最后一行显示当前模式与模型。
///
/// @note 本控件自绘，不调用 tui::InputBox::render，因此基类内部的横竖滚动量恒为 0；
///       cursor() 借这一点从基类光标反推逻辑行号与显示列（基类没有公开它们）。
class PromptBox final : public tui::InputBox {
public:
    static constexpr int k_max_rows = 8; ///< 正文最多占的行数，超出后在框内滚动

    void set_footer(std::string);
    void set_placeholder(std::string);
    void set_active(bool);
    void set_theme(const tui::ThemeTokens&);

    tui::Size measure(tui::Size available) const override;
    std::optional<tui::Point> cursor() const override;
    void render(tui::Surface&) override;

private:
    /// 折行后的一行：属于哪个逻辑行，以及在整段文本里的字节区间。
    struct Row {
        std::size_t line = 0, begin = 0, end = 0;
    };

    static std::vector<Row> wrap(std::string_view value, int width);
    /// 光标所在的折行行号与该行内的显示列；没有光标时返回 nullopt。
    std::optional<std::pair<int, int>> caret(const std::vector<Row>& rows,
                                             std::string_view value) const;
    static int text_rows(int height) noexcept; ///< 去掉底部一行后的正文高度
    int inner_width(int cols) const noexcept;

    std::string footer_;
    std::string placeholder_ = std::string(ui::text().box_placeholder);
    bool active_ = false;
    const tui::ThemeTokens* theme_ = &tui::dark_theme();
};

} // namespace dagent::ui
