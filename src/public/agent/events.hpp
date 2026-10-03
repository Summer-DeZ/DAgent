/// @file events.hpp
/// @brief 核心的实时出口：执行事件（Sink）、审批（Approver）与问答（Asker）的中立值类型。
/// runtime 把它们接到交互代理与后端事件发布；前端只经协议 DTO 看到它们。
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agent/grant.hpp"
#include "agent/intent.hpp"
#include "agent/reply.hpp"
#include "agent/tool_data.hpp"
#include "lib/nlohmann/json.hpp"

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

// TextDelta、ReasoningDelta：见 reply.hpp

struct StreamReset {}; ///< 这一步要重来：丢弃这一步已显示的内容

struct ToolPending {
    std::string id, name; ///< 流里出现了一个工具调用，参数还在传
};

struct ToolStarted {
    std::string id, name, summary;
    ExecutionGrant grant;
};

struct ToolOutput {
    std::string id, chunk; ///< bash 实时输出（原始块）
};

struct ToolFinished {
    std::string id, name, summary;
    ToolResult result;
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
    bool persistent = false; ///< Keep audit information visible in the transcript.
};

struct TurnEnded {
    TurnStatus status = TurnStatus::done;
    std::string error;
    int steps = 0, tool_calls = 0;
    Usage total;
};

struct EventBox; // 前向声明，定义在 Event 之后（variant 的 alternative 必须是完整类型）

/// @brief 子 Agent 事件的信封：父 Sink 收到它时，内含的是子会话里真正发生的事。
struct SubEvent {
    std::string session;   ///< 子会话 id
    std::string agent;     ///< 子 Agent 名
    std::string call_id;   ///< 父会话里那次 task 调用的 id
    std::shared_ptr<const EventBox> boxed;

    /// 内联实现在 EventBox 定义之后；Event 是 alias，无法前置声明，故用占位返回类型。
    const auto& event() const;
};

using Event = std::variant<TurnStarted, StepStarted, TextDelta, ReasoningDelta, StreamReset, ToolPending,
                           ToolStarted, ToolOutput, ToolFinished, SubEvent, Retrying, Compacted,
                           ContextUpdate, Notice, ModelChanged, ModeChanged, TurnEnded>;
using Sink = std::function<void(const Event&)>;

struct EventBox { Event event; }; // 此处 Event 已完整

inline const auto& SubEvent::event() const { return boxed->event; }

// ---- 权限询问 ----

struct Approval {
    struct Request {
        enum class Kind {
            dynamic_command,
            read_path,
            write_path,
            network,
            sensitive_read,
            protected_write,
            host_access,
        };
        Kind kind = Kind::dynamic_command;
        std::string target, reason;
    };
    std::string call_id, tool;
    std::string arguments; ///< 原始工具参数；只供审阅，不作为执行入口
    ApprovalAuthority authority = ApprovalAuthority::user;
    ApprovalIdentity identity;
    std::stop_token authority_stop;
    std::string delegated_task;
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
    PreparedIntent intent;    ///< 拷贝：交互界面要把它 post 到渲染线程
    std::string reason;       ///< 为什么要问，见 docs/design/agent.md §7
    std::string session_rule; ///< 选「本会话允许」会记住什么，给界面显示；为空表示不提供这个选项
    std::string agent;          ///< 来源子 Agent 名；主 Agent 自己的审批为空
    std::string origin_call_id; ///< 父会话里那次 task 调用的 id；主 Agent 为空
    std::string cwd, mode;
    bool read_only = false, planning = false;
    std::vector<std::string> existing_permissions;
    std::vector<Request> requests;
    bool partially_executed = false;
};

struct Decision {
    enum class Answer { allow, allow_session, deny, deny_with_feedback };
    Answer answer = Answer::deny;
    std::string feedback; ///< 拒绝反馈或批准理由
    std::string state; ///< approved / denied / cancelled / expired；空为旧用户答复
    bool explicit_denial = false; ///< 确实作出拒绝决定；故障/预算/缺少审批出口不写永久 deny
    std::string model; ///< 实际审阅模型配置名
    Usage usage; ///< 只在父侧计费；子记录保留关联证据
    std::size_t estimated_budget_tokens = 0; ///< Provider 未报告 usage 的审阅尝试，预算估算（不冒充实测用量）
};

using Approver = std::function<Decision(Approval&, std::stop_token)>;

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

/// @brief 事件 → 原实时 JSON 形状（backend 转成协议事件 data；run 的 jsonl 由前端还原同一形状）。
nlohmann::json to_json(const Event&);

std::string_view to_string(TurnStatus); ///< "done" / "interrupted" / …

} // namespace dagent::agent
