/// @file run.hpp
/// @brief 一次 turn 或 compact 的运行状态：阶段、计数、取消与唯一结束结果。
///
/// Run 只在本后端实例内有效，不落库（状态机 §3）。阶段只描述进度，不创建第二套结束状态机。
#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

#include "agent/events.hpp"
#include "agent/identity.hpp"
#include "agent/reply.hpp"
#include "agent/skills.hpp"

namespace dagent::agent {

enum class RunKind { turn, compact };

/// 运行阶段；子 Run 有自己的阶段，父等待子组时为 waiting_children。
enum class RunPhase {
    ready,
    preparing_context,
    requesting_model,
    dispatching,
    waiting_approval,
    waiting_question,
    waiting_children,
    compacting,
    finalizing,
    finished,
};

/// Run 的唯一结束结果：包含运行类型、TurnStatus、计数与用量。
struct RunOutcome {
    RunKind kind = RunKind::turn;
    TurnStatus status = TurnStatus::done;
    std::string error;
    int steps = 0, tool_calls = 0;
    Usage usage;
};

class Run {
public:
    Run(RunKind kind, RunId id) : kind_(kind), id_(std::move(id)) {}
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    RunSkills& skills() { return skills_; }
    const RunSkills& skills() const { return skills_; }

    const RunId& id() const { return id_; }
    RunKind kind() const { return kind_; }
    RunPhase phase() const { return phase_.load(); }
    void enter_phase(RunPhase phase) { phase_.store(phase); }

    /// @brief 绑定外部取消链：外部 stop 触发时同步取消本 Run；已经触发时立即取消。
    void begin(std::stop_token external) {
        external_.emplace(external, [this] { request_cancel(); });
    }

    /// @brief 幂等：只触发 stop_source，不伪造终态。
    void request_cancel() { stop_.request_stop(); }
    bool cancel_requested() const { return stop_.stop_requested(); }
    std::stop_token stop_token() const { return stop_.get_token(); }

    void count_step() { ++steps_; }
    int steps() const { return steps_; }
    void count_calls(int calls) { tool_calls_ += calls; }
    int tool_calls() const { return tool_calls_; }
    void add_usage(const Usage& usage) {
        total_.prompt += usage.prompt;
        total_.completion += usage.completion;
        total_.cached += usage.cached;
    }
    const Usage& usage() const { return total_; }

    /// @brief 达到工具预算后的最终总结机会。
    void set_grace(bool value) { grace_ = value; }
    bool grace() const { return grace_; }

    /// @brief 唯一收尾入口，只能成功一次；调用后只允许读取结果。
    RunOutcome finish(TurnStatus status, std::string error) {
        if (finished_) return outcome_;
        finished_ = true;
        skills_ = {};
        enter_phase(RunPhase::finished);
        outcome_.kind = kind_;
        outcome_.status = status;
        outcome_.error = std::move(error);
        outcome_.steps = steps_;
        outcome_.tool_calls = tool_calls_;
        outcome_.usage = total_;
        return outcome_;
    }
    bool finished() const { return finished_; }

private:
    RunSkills skills_;
    RunKind kind_;
    RunId id_;
    std::atomic<RunPhase> phase_ = RunPhase::ready;
    std::stop_source stop_;
    std::optional<std::stop_callback<std::function<void()>>> external_;
    int steps_ = 0, tool_calls_ = 0;
    Usage total_;
    bool grace_ = false;
    bool finished_ = false;
    RunOutcome outcome_;
};

} // namespace dagent::agent
