/// @file projection.hpp
/// @brief UI 侧投影类型：协议事件/历史条目 → 页面可消费的值。
///
/// UI 只依赖 protocol 的线上形状，不包含 agent/tools/storage/llm 头；
/// 视图 JSON 按 ToolPresentation 语义解析，渲染方式（折叠/颜色/布局）由 UI 决定。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "lib/nlohmann/json.hpp"
#include "protocol/dto.hpp"

namespace dagent::ui {

enum class TurnStatus { done, interrupted, denied, limit, failed };

enum class NoticeLevel { info, warn, error };

// ---- 工具/控制动作的展示事实（原 agent::ToolData 的 UI 投影）----

struct TodoItem {
    enum class State { todo, doing, done, dropped };
    std::string text;
    State state = State::todo;

    bool operator==(const TodoItem&) const = default;
};

struct ReadView {
    std::string path;
    int start_line = 0, end_line = 0;
    bool directory = false;
};

struct FileChangeView {
    std::string path, diff;
    int added = 0, removed = 0;
    bool created = false;
};

struct BashView {
    std::string command, output;
    bool interrupted = false, timed_out = false;
    std::optional<int> exit_code, signal;
    int elapsed_ms = 0;
};

struct GrepLine {
    std::string path, text;
    int line = 0;
};

struct GrepView {
    std::string pattern;
    std::vector<GrepLine> lines;
};

struct GlobView {
    std::string pattern;
    std::vector<std::string> files;
};

struct McpView {
    std::string server, tool;
    std::vector<nlohmann::json> content;
};

struct AskOption {
    std::string label, description;
};

struct AskView {
    std::string header, prompt;
    std::vector<AskOption> options;
    std::vector<int> selected;
    std::string other;
    bool cancelled = false;
};

struct TaskStep {
    std::string summary;
    bool is_error = false;
};

struct TaskView {
    std::string agent, session_id, result;
    std::vector<TaskStep> steps;
    int tool_calls = 0;
    double seconds = 0;
};

struct SkillView { std::string name, path; };

using TodoList = std::vector<TodoItem>;
using ToolView = std::variant<std::monostate, ReadView, FileChangeView, BashView, GrepView, GlobView,
                              McpView, TodoList, AskView, TaskView, SkillView>;

struct ToolFinished {
    std::string id, name, summary, text;
    bool is_error = false, interrupted = false;
    ToolView view;
};

// ---- 实时事件（原 agent::Event 的 UI 投影）----

struct TurnStarted {
    std::string input;
};
struct StepStarted {};
struct TextDelta {
    std::string text;
};
struct ReasoningDelta {
    std::string text;
};
struct StreamReset {};
struct ToolPending {
    std::string name;
};
struct ToolStarted {
    std::string id, name, summary;
};
struct ToolOutput {
    std::string id, chunk;
};
struct Retrying {
    int attempt = 0, max_attempts = 0;
    std::int64_t wait_ms = 0;
    std::string reason;
};
struct Compacted {
    std::size_t before = 0, after = 0;
};
struct ContextUpdate {
    std::size_t used = 0, limit = 0;
};
struct Notice {
    NoticeLevel level = NoticeLevel::info;
    std::string text;
    bool persistent = false;
};
struct ModelChanged {
    std::string model;
};
struct ModeChanged {
    std::string mode;
    bool planning = false;
};
struct TurnEnded {
    TurnStatus status = TurnStatus::done;
    std::string error;
};

using EventPayload =
    std::variant<TurnStarted, StepStarted, TextDelta, ReasoningDelta, StreamReset, ToolPending,
                 ToolStarted, ToolOutput, ToolFinished, Retrying, Compacted, ContextUpdate,
                 Notice, ModelChanged, ModeChanged, TurnEnded>;
struct Event {
    EventPayload payload;
};
/// @brief 协议实时事件 → UI 事件；身份由调用方路由，不显示的通知返回 nullopt。
std::optional<Event> decode_event(const protocol::Event& event);

/// @brief 历史条目 → 静态 UI 事件；一条 assistant 可产生思考与正文两个事件。
std::vector<Event> decode_history(const protocol::HistoryItem& item);

// ---- 审批 / 问答 / 模型表单 / 状态 ----

struct ApprovalItem {
    std::string kind, target, reason;
};

struct ApprovalRequest {
    std::string tool, agent, reason, summary;
    std::string preview_kind = "text", preview_text;
    std::string cwd, mode, session_rule;
    std::vector<ApprovalItem> requests;
    bool partially_executed = false;
};

struct QuestionRequest {
    std::string header, prompt;
    std::vector<AskOption> options;
    bool multi_select = false, allow_other = true;
};

struct ApprovalAnswer {
    enum class Decision { allow, allow_session, deny, deny_with_feedback };
    Decision decision = Decision::deny;
    std::string feedback;
};

struct QuestionAnswer {
    std::vector<int> selected;
    std::string other;
    bool cancelled = false;
};

struct ProviderKind {
    std::string kind, default_base_url;
    bool needs_credential = true;
};

struct ModelInput {
    std::string kind, name, base_url, model, credential;
    std::size_t max_tokens = 8192;
    std::size_t context_window = 0;
};

struct McpStatus {
    std::string name, status, error, boundary;
    std::size_t tools = 0;
};

ApprovalRequest decode_approval(const nlohmann::json& payload);
QuestionRequest decode_question(const nlohmann::json& payload);
std::vector<McpStatus> decode_mcp(const nlohmann::json& array);

} // namespace dagent::ui
