/// @file turn_meter.hpp
/// @brief 回合耗时、流式输出量及速率的值类型，不持有控件、计时器或 Runtime。
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace dagent::ui {

class TurnMeter {
public:
    struct Display {
        double seconds;
        std::string tokens;
        std::string rate;
    };

    /// 每个 TurnStarted 都重置计量，保留当前阶段。
    void start_turn();
    /// Shell 从空闲转为 busy 时调用，同时将阶段设为 Working。
    void start_activity();
    void start_step();
    void tool_pending();
    void reset_stream();
    void streamed(std::string_view text);
    void complete_call(std::optional<std::size_t> output);
    void retry();

    void set_phase(std::string phase);
    const std::string& phase() const { return phase_; }
    /// 读取当前时刻的计量并沿用 UI 文案格式；零输出或无流式用时的指标留空。
    Display display() const;

private:
    using Clock = std::chrono::steady_clock;
    void reset_counts();

    Clock::time_point turn_begin_{};
    std::optional<Clock::time_point> streaming_;
    double stream_seconds_ = 0;
    std::size_t output_ = 0, estimated_ = 0, stream_chars_ = 0;
    std::string phase_;
};

} // namespace dagent::ui
