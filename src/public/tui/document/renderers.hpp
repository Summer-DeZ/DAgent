/// @file renderers.hpp
/// @brief 内置块渲染器：文本、差异、Markdown、表格与语法高亮。
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "tui/document/document.hpp"

namespace dagent::tui {

/// @brief 文本渲染器：每行一段，样式取指定的 ThemeTokens 槽位。
class TextRenderer final : public BlockRenderer {
public:
    explicit TextRenderer(Style ThemeTokens::*slot = &ThemeTokens::text) noexcept
        : slot_(slot) {}

    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;

private:
    Style ThemeTokens::*slot_;
};

/// @brief 差异渲染器：'+' / '-' / '@' 开头的行用 diff_* 令牌，其余正文。
class DiffRenderer final : public BlockRenderer {
public:
    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;
};

/// @brief Markdown 渲染器：隐藏行内标记，按显示文本折行。
class MarkdownRenderer final : public BlockRenderer {
public:
    WrapResult measure(std::string_view source, size_t from,
                       int width) const override;
    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;
};

/// @brief 表格渲染器：列宽依赖全部行，每次变化整块重排。
class TableRenderer final : public BlockRenderer {
public:
    WrapResult measure(std::string_view source, size_t from,
                       int width) const override;
    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;
};

/// @brief 语法高亮渲染器：语言取自 Block::meta 的首个词，左侧留 k_gutter 列。
class SyntaxRenderer final : public BlockRenderer {
public:
    static constexpr int k_gutter = 2;

    WrapResult measure(std::string_view source, size_t from, int width) const override;
    size_t render(const Block& block, int width, const ThemeTokens& theme, size_t from,
                  size_t valid, std::vector<Line>& out) const override;
};

} // namespace dagent::tui
