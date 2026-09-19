/// @file view.hpp
/// @brief 工具结果的界面数据：每个工具一个结构体，给界面显示，也存进会话供 --resume 重放。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "lib/nlohmann/json.hpp"

namespace dagent::tools {

struct ReadView {
    std::string path;
    int start_line = 0, end_line = 0, total_lines = 0;
    bool truncated = false, directory = false;
};

/// edit 和 write 共用，界面按同一种方式画 diff。
struct FileChangeView {
    std::string path, diff; ///< diff 是 unified_diff 的文本
    int added = 0, removed = 0;
    bool created = false;
};

struct BashView {
    std::string command, output; ///< output 是 exec 截断后的完整输出，不是给模型的那份
    std::optional<int> exit_code, signal;
    bool timed_out = false, interrupted = false;
    std::string sandbox; ///< "read_only" / "workspace_write" / "full_access"
    std::int64_t elapsed_ms = 0;
};

struct GrepLine {
    std::string path, text;
    std::uint64_t line = 0;
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    bool is_context = false;
};

struct GrepView {
    std::string pattern;
    std::vector<GrepLine> lines;
    bool truncated = false;
};

struct GlobView {
    std::string pattern;
    std::vector<std::string> files;
    bool truncated = false;
};

struct McpView {
    std::string server, tool;
    std::vector<nlohmann::json> content; ///< 内容块原样保留，界面自己决定怎样显示
    nlohmann::json structured;
    bool disconnected = false;
};

/// monostate：prepare 阶段就失败的调用（参数错误等），界面只显示 text。
using View = std::variant<std::monostate, ReadView, FileChangeView, BashView, GrepView, GlobView, McpView>;

/// 序列化成 {"kind": "read", ...}，给 session::Writer::append；kind 区分各分支。
nlohmann::json to_json(const View&);

/// 回放时用；kind 缺失或不认识时返回 monostate。
View view_from_json(const nlohmann::json&);

} // namespace dagent::tools
