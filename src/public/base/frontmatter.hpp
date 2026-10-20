/// @file frontmatter.hpp
/// @brief 只切分 frontmatter 分隔符，不解析其中的内容。
#pragma once

#include <expected>
#include <string_view>

namespace dagent::base {

struct Frontmatter {
    std::string_view header;
    std::string_view body;
};

enum class FrontmatterError { missing_opening, missing_closing };

/// @brief 输入须已将换行规范化为 LF，且以 "---\n" 开始。
/// 首个内容恰为 "---" 的后续行结束 header；结尾分隔符可位于文件末尾而无换行。
/// 返回视图不含两条分隔符及其换行，其余内容原样保留；视图借用输入文本的存储。
std::expected<Frontmatter, FrontmatterError> split_frontmatter(std::string_view text);

} // namespace dagent::base
