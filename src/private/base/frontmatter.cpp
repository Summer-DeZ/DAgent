#include "base/frontmatter.hpp"

namespace dagent::base {

std::expected<Frontmatter, FrontmatterError> split_frontmatter(std::string_view text) {
    if (!text.starts_with("---\n")) return std::unexpected(FrontmatterError::missing_opening);
    std::size_t end = 4;
    while (end < text.size()) {
        const auto newline = text.find('\n', end);
        const auto last = newline == std::string_view::npos ? text.size() : newline;
        if (text.substr(end, last - end) == "---") {
            const auto body = newline == std::string_view::npos ? last : newline + 1;
            return Frontmatter{text.substr(4, end - 4), text.substr(body)};
        }
        if (newline == std::string_view::npos) break;
        end = newline + 1;
    }
    return std::unexpected(FrontmatterError::missing_closing);
}

} // namespace dagent::base
