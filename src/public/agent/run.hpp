/// @file run.hpp
/// @brief 一次 turn 或 compact 的运行状态：计数、取消与唯一结束结果。
///
/// Run 只在本后端实例内有效，不落库（状态机 §3）。
#pragma once

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

/// 本轮结束结果：状态、计数与用量。
struct RunOutcome {
    TurnStatus status = TurnStatus::done;
    std::string error;
    int steps = 0, tool_calls = 0;
    Usage usage;
};

class Run {
public:
    explicit Run(RunId id) : id_(std::move(id)) {}
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    RunSkills& skills() { return skills_; }
    const RunSkills& skills() const { return skills_; }

    const RunId& id() const { return id_; }

    /// @brief 绑定外部取消链：外部 stop 触发时同步取消本 Run；已经触发时立即取消。
    void begin(std::stop_token external) {
        external_.emplace(external, [this] { request_cancel(); });
    }

    /// @brief 幂等：只触发 stop_source，不伪造终态。
    void request_cancel() { stop_.request_stop(); }
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
        outcome_.status = status;
        outcome_.error = std::move(error);
        outcome_.steps = steps_;
        outcome_.tool_calls = tool_calls_;
        outcome_.usage = total_;
        return outcome_;
    }

private:
    RunSkills skills_;
    RunId id_;
    std::stop_source stop_;
    std::optional<std::stop_callback<std::function<void()>>> external_;
    int steps_ = 0, tool_calls_ = 0;
    Usage total_;
    bool grace_ = false;
    bool finished_ = false;
    RunOutcome outcome_;
};

} // namespace dagent::agent
