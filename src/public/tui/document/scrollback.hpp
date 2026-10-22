/// @file scrollback.hpp
/// @brief 大内容滚动区控件：Document 的门面 widget，负责滚动、绘制与选择。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "tui/document/document.hpp"
#include "tui/widget/widget.hpp"

namespace dagent::tui {

/// @brief 大内容滚动区：Document 的门面 widget。
class Scrollback : public Widget {
public:
    Document& document() noexcept { return doc_; }
    const Document& document() const noexcept { return doc_; }

    /// @brief 换主题（更改样式后需递增 ThemeTokens::epoch）。
    void set_theme(ThemeTokens theme) {
        theme_ = std::move(theme);
        invalidate();
    }
    const ThemeTokens& theme() const noexcept { return theme_; }

    // ---- 滚动（正数 = 向下/新内容方向） ----
    void scroll_lines(int lines);
    void scroll_pages(int pages);
    void scroll_home();
    void scroll_end(); ///< 贴底并保持跟随

    [[nodiscard]] bool pinned() const noexcept {
        return doc_.anchor().pinned_to_bottom;
    }
    /// @brief 视口下方的行数（不贴底时 > 0）。
    [[nodiscard]] size_t unseen_rows() const noexcept { return unseen_; }

    /// @brief 块内容是否出现在最近绘制的视口。
    bool block_visible(uint64_t id) const noexcept {
        return doc_.block_visible(id, top_, static_cast<size_t>(view_));
    }

    // ---- 选择 ----

    /// @brief 视图坐标 → 内容位置；文档为空时返回 nullopt。
    std::optional<Location> hit(Point p);
    void select(Selection s);
    void clear_selection();
    [[nodiscard]] const std::optional<Selection>& selection() const noexcept {
        return selection_;
    }
    /// @brief 选区的源文本；无选区时为空串。
    [[nodiscard]] std::string selected_text() const;

    Size measure(Size available) const override;
    void render(Surface& s) override;
    bool dirty_tree() const noexcept override;

private:
    void draw_row(Surface& s, int y, size_t row, const Line& ln);
    size_t max_top() const noexcept;
    /// @brief 把视口顶行锚到绝对行 row；dir < 0 表示向上滚动。
    void anchor_to(size_t row, int dir);

    Document doc_;
    ThemeTokens theme_ = dark_theme();
    uint64_t rendered_rev_ = 0; ///< 已渲染到的内容版本
    size_t total_ = 0;          ///< 上次渲染的文档总行数
    size_t top_ = 0;            ///< 上次渲染的视口顶行
    size_t unseen_ = 0;         ///< 视口下方行数
    int view_ = 0;              ///< 上次渲染的视口高度
    std::optional<Selection> selection_;
};

} // namespace dagent::tui
