/// @file events.hpp
/// @brief 核心的对外接口：agent 发出事件（Sink），需要时询问权限（Approver），一轮结束时返回状态。
/// 交互界面、run 模式和以后的任何前端都只实现这三样东西。
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agent/llm.hpp"
#include "lib/nlohmann/json.hpp"
#include "tools/tools.hpp"

namespace dagent::agent {

/// @brief 一轮的结束状态。
enum class TurnStatus {
    done,        ///< 模型给出了不带工具调用的回复
    interrupted, ///< stop 被请求
    denied,      ///< 用户拒绝了某个调用（不带说明），停下来等新指示
    limit,       ///< 达到 max_model_calls 或 max_tool_calls
    failed,      ///< 模型不可用、上下文压不下来等，详见 TurnEnded::error
};

// ---- 事件 ----

struct TurnStarted {
    std::string input;
};

struct StepStarted {
    int step = 0; ///< 第几次模型请求，从 1 开始
};

// TextDelta、ReasoningDelta：复用 llm.hpp 的类型

struct StreamReset {}; ///< 这一步要重来：丢弃这一步已显示的内容

struct ToolPending {
    std::string id, name; ///< 流里出现了一个工具调用，参数还在传
};

struct ToolStarted {
    std::string id, name, summary;
    tools::Grant grant;
};

struct ToolOutput {
    std::string id, chunk; ///< bash 实时输出（原始块）
};

struct ToolFinished {
    std::string id, name, summary;
    tools::Result result;
};

struct Retrying {
    int attempt = 0, max_attempts = 0;
    std::chrono::milliseconds wait{0};
    std::string reason;
};

struct Compacted {
    std::size_t before = 0, after = 0; ///< 估算 tokens，压缩前后
    bool summarized = false;
};

struct ModelChanged { std::string model; }; ///< 会话回放及切换时更新后续消息的模型标签
struct ModeChanged { std::string mode; bool planning = false; };

struct ContextUpdate {
    Usage usage;
    std::size_t used = 0, limit = 0; ///< 每步结束后
};

struct Notice {
    enum class Level { info, warn, error };
    Level level = Level::info;
    std::string text;
};

struct TurnEnded {
    TurnStatus status = TurnStatus::done;
    std::string error;
    int steps = 0, tool_calls = 0;
    Usage total;
};

using Event = std::variant<TurnStarted, StepStarted, TextDelta, ReasoningDelta, StreamReset, ToolPending,
                           ToolStarted, ToolOutput, ToolFinished, Retrying, Compacted, ContextUpdate,
                           Notice, ModelChanged, ModeChanged, TurnEnded>;
using Sink = std::function<void(const Event&)>;

// ---- 权限询问 ----

struct Approval {
    std::string call_id, tool;
    tools::Intent intent;     ///< 拷贝：交互界面要把它 post 到渲染线程
    std::string reason;       ///< 为什么要问，见 docs/design/agent.md §7
    std::string session_rule; ///< 选「本会话允许」会记住什么，给界面显示；为空表示不提供这个选项
    bool can_network = false; ///< bash：是否提供「允许并联网」
};

struct Decision {
    enum class Answer { allow, allow_session, deny, deny_with_feedback };
    Answer answer = Answer::deny;
    std::string feedback; ///< deny_with_feedback 时用户写的说明
    bool network = false; ///< 只在 can_network 时有意义
};

using Approver = std::function<Decision(const Approval&, std::stop_token)>;

struct Question {
    struct Option { std::string label, description; };
    std::string call_id;
    std::string header;
    std::string prompt;
    std::vector<Option> options;
    bool multi_select = false;
    bool allow_other = true;
};

struct Answer {
    std::vector<int> selected;
    std::string other;
    bool cancelled = false;
};

using Asker = std::function<Answer(const Question&, std::stop_token)>;

/// @brief 事件 → JSON（`--output jsonl` 的每一行）。
nlohmann::json to_json(const Event&);

std::string_view to_string(TurnStatus); ///< "done" / "interrupted" / …

} // namespace dagent::agent
