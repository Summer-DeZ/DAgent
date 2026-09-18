/// @file search.hpp
/// @brief 代码搜索：用 ripgrep 做 grep 与文件列举，另有给 @ 补全用的模糊匹配。
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "workspace/error.hpp"

namespace dagent::workspace {

/// @brief 搜索选项。rg 缺失时抛 tool_missing。
struct SearchOptions {
    std::filesystem::path rg_path; ///< 为空时在 PATH 里找 rg，并把结果缓存
};

struct GrepQuery {
    std::string pattern;
    std::filesystem::path root;
    std::vector<std::string> globs;      ///< --glob，可以写 "!*.lock" 表示排除
    std::optional<std::string> type;     ///< --type cpp
    bool fixed_strings = false;
    bool case_insensitive = false;       ///< 不设时用 --smart-case
    bool multiline = false;
    bool hidden = false;                 ///< 是否包含隐藏文件；.git 目录始终排除
    int context = 0;                     ///< -C
    std::size_t max_matches = 200;       ///< 全局总数上限
};

struct Match {
    std::string path; ///< 相对 root
    std::uint64_t line = 0;
    std::string text; ///< 已去掉行尾换行
    std::vector<std::pair<std::size_t, std::size_t>> spans; ///< 匹配位置（字节偏移），给 UI 高亮用
    bool is_context = false;
};

struct GrepResult {
    std::vector<Match> matches;
    std::size_t files_with_matches = 0;
    bool truncated = false;
};

/// @brief 执行搜索。没有匹配返回空结果（exit 1 是正常情况）；正则错误抛 bad_pattern 并带上 rg 的报错。
GrepResult grep(const GrepQuery&, const SearchOptions& = {}, std::stop_token = {});

struct FilesQuery {
    std::filesystem::path root;
    std::vector<std::string> globs;
    bool hidden = false;
    bool sort_by_mtime = false; ///< 最近修改的排在前面
    std::size_t max_files = 1000;
};

/// @brief 列举文件，遵守 .gitignore 等忽略规则。路径相对 root。
std::vector<std::string> files(const FilesQuery&, const SearchOptions& = {}, std::stop_token = {});

/// @brief 给 @ 文件补全用：fzy 打分，返回 haystack 下标（按分数从高到低）。
/// 输入 "tuidoc" 能命中 "src/private/tui/document.cpp"。
std::vector<std::size_t> fuzzy_rank(std::string_view needle, std::span<const std::string> haystack,
                                    std::size_t limit);

} // namespace dagent::workspace
