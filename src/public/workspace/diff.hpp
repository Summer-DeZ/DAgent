/// @file diff.hpp
/// @brief 生成 unified diff 与增删统计。不做打补丁（那属于工具语义）。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace dagent::workspace {

struct DiffStat {
    std::size_t added = 0, removed = 0;
};

struct DiffOptions {
    int context = 3;
    std::size_t max_lines = 20000; ///< 两边都超过这个行数时不做逐行比较，按整体替换处理
};

struct Unified {
    std::string text; ///< 以 "--- a/<path>\n+++ b/<path>\n" 开头；没有差异时为空
    DiffStat stat;
    bool whole_file = false; ///< 放弃逐行比较，按整体替换处理
};

/// @brief 输入应该是 LF 文本。新建/删除文件分别用 --- /dev/null 和 +++ /dev/null。
Unified unified_diff(std::string_view before, std::string_view after, std::string_view path,
                     const DiffOptions& = {});

} // namespace dagent::workspace
