#include "ui/state/turn_meter.hpp"

#include <algorithm>
#include <utility>

#include "ui/style/display.hpp"
#include "ui/style/strings.hpp"

namespace dagent::ui {

void TurnMeter::reset_counts() {
    output_ = estimated_ = stream_chars_ = 0;
    streaming_.reset();
    stream_seconds_ = 0;
}

void TurnMeter::start_turn() {
    turn_begin_ = Clock::now();
    reset_counts();
}

void TurnMeter::start_activity() {
    turn_begin_ = Clock::now();
    phase_ = std::string(ui::text().activity_working);
    reset_counts();
}

void TurnMeter::start_step() {
    phase_ = std::string(ui::text().activity_working);
    stream_chars_ = 0;
}

void TurnMeter::tool_pending() {
    phase_ = std::string(ui::text().activity_working);
    if (!streaming_) streaming_ = Clock::now();
}

void TurnMeter::reset_stream() {
    stream_chars_ = 0;
    streaming_.reset();
}

void TurnMeter::streamed(std::string_view text) {
    if (text.empty()) return;
    phase_ = std::string(ui::text().activity_working);
    if (!streaming_) streaming_ = Clock::now();
    // 四个 Unicode 码点约一个 token；是显示估计，收到 usage 后即替换。
    stream_chars_ += std::ranges::count_if(text, [](unsigned char c) { return (c & 0xc0) != 0x80; });
}

void TurnMeter::complete_call(std::optional<std::size_t> output) {
    if (streaming_) stream_seconds_ += std::chrono::duration<double>(Clock::now() - *streaming_).count();
    streaming_.reset();
    if (output) output_ += *output;
    else estimated_ += (stream_chars_ + 3) / 4;
    stream_chars_ = 0;
}

void TurnMeter::retry() {
    phase_ = std::string(ui::text().activity_retrying);
    streaming_.reset();
}

void TurnMeter::set_phase(std::string phase) { phase_ = std::move(phase); }

TurnMeter::Display TurnMeter::display() const {
    const auto now = Clock::now();
    const auto seconds = std::chrono::duration<double>(now - turn_begin_).count();
    const auto count = output_ + estimated_ + (stream_chars_ + 3) / 4;
    const double streaming = stream_seconds_ + (streaming_ ? std::chrono::duration<double>(now - *streaming_).count() : 0);
    const auto tokens = std::string(estimated_ || streaming_ || stream_chars_ ? "~" : "") + compact_tokens(count);
    return {seconds,
            count > 0 ? format_text(ui::text().activity_tokens, tokens) : "",
            count > 0 && streaming > 0 ? format_text(ui::text().activity_rate, count / streaming) : ""};
}

} // namespace dagent::ui
