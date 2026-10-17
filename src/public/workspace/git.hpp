/// @file git.hpp
/// @brief 只读取证的 Git 原语：单文件相对 HEAD 的差异。
///
/// 取证不使用外部 diff/textconv，避免生成审批证据时执行仓库自定义程序。
/// 不在仓库中、无 HEAD、二进制等失败情形返回 error，由调用方决定退回全文或转审批。
#pragma once

#include <cstddef>
#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>

namespace dagent::workspace {

struct HeadDiff {
    /// 文件是否被跟踪：false 表示未跟踪（调用方按整体新增处理）。
    bool tracked = false;
    /// 原始 unified diff；tracked 且无改动时为空。
    std::string text;
    /// diff 正文行数（含上下文，不含文件头与 hunk 定位头）。
    std::size_t content_lines = 0;
    /// 失败说明；非空表示无法取证（不在仓库、无 HEAD、二进制、不可读等）。
    std::string error;
};

/// @brief 取单个文件相对 HEAD 的原始差异。
/// @param file 目标文件的绝对路径。
HeadDiff head_diff(const std::filesystem::path& file,
                   std::chrono::steady_clock::time_point deadline, std::stop_token stop);

} // namespace dagent::workspace
