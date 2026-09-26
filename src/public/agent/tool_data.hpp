/// @file tool_data.hpp
/// @brief 工具结果与展示的业务事实：给模型的文本、错误/中断语义、结构化展示数据、执行信号。
///
/// 这些结构是核心与历史记录共用的中立值；View JSON 的编码（to_json / view_from_json）
/// 也定义在这里，保持与会话记录的字段和 kind 名称一致。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "lib/nlohmann/json.hpp"

namespace dagent::agent {

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
    std::string backend, grant_source;
    int analysis_version = 0;
    bool allow_network = false; ///< 回放时保留实际的联网授权
    bool allow_local_sockets = false, private_tmp = false;
    bool protect_sensitive_names = false;
    std::vector<std::string> readable, writable, protected_read, protected_write, network_targets;
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

struct TodoItem {
    enum class State : std::uint8_t { todo, doing, done, dropped };

    std::string text;
    State state = State::todo;
    bool operator==(const TodoItem&) const = default;
};

/// @brief AI 当前的整份计划；每次工具调用都完整替换上一份。
struct TodoView {
    std::vector<TodoItem> items;
    bool operator==(const TodoView&) const = default;
};

struct AskOption {
    std::string label, description;
};

struct AskView {
    std::string header, prompt;
    std::vector<AskOption> options;
    std::vector<int> selected;
    std::string other;
    bool multi_select = false;
    bool allow_other = true;
    bool cancelled = false;
};

/// @brief 子 Agent 一次工具调用的摘要（task 视图里逐条列出）。
struct TaskStep {
    std::string summary;
    bool is_error = false;
    bool operator==(const TaskStep&) const = default;
};

/// @brief task 工具的结果视图：session_id 是父到子的唯一跳转锚点。
struct TaskView {
    std::string agent;            ///< 子 Agent 名
    std::string task;             ///< 任务 prompt
    std::string session_id;       ///< 子会话 id
    std::string result;           ///< 子 Agent 的最终文本
    std::vector<TaskStep> steps;  ///< 逐条工具摘要
    int model_calls = 0;          ///< 子 Agent 的模型请求数
    int tool_calls = 0;           ///< 子 Agent 的工具调用数
    double seconds = 0;
    bool interrupted = false;
    bool operator==(const TaskView&) const = default;
};

/// monostate：prepare 阶段就失败的调用（参数错误等），界面只显示文本。
using View = std::variant<std::monostate, ReadView, FileChangeView, BashView, GrepView, GlobView,
                          McpView, TodoView, AskView, TaskView>;

/// @brief 执行信号：不是展示事实，而是执行层需要核心处理的事件。
struct McpDisconnected {
    std::string server; ///< 断连的 MCP server 名
};
using ExecutionSignal = std::variant<McpDisconnected>;

/// @brief 一次工具调用的完整结果。
struct ToolResult {
    std::string model_text;   ///< 给模型：合法 UTF-8，已按 max_result_bytes 截断
    bool is_error = false;    ///< 模型视角的失败：参数错、找不到、匹配失败、退出码非 0……
    bool interrupted = false; ///< stop_token 触发；model_text 里是已有的部分输出
    View display;             ///< 给界面与会话的展示事实，按 view JSON 格式编码
    std::vector<ExecutionSignal> signals; ///< 执行信号（如 MCP 断连），不由 display 分支判定
};

/// 序列化成 {"kind": "read", ...}，给会话记录与实时事件；kind 区分各分支。
nlohmann::json to_json(const View&);

/// 回放时按当前格式读取；kind 为 null 返回 monostate，缺字段或未知 kind 抛 RecordError。
View view_from_json(const nlohmann::json&);

} // namespace dagent::agent
