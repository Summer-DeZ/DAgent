/// @file output.hpp
/// @brief run 模式的输出适配：协议事件 → text/json/jsonl 格式。
///
/// 各格式共用主会话结果累积；JSONL 直接输出 protocol::Event 信封。
#pragma once

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <mutex>
#include <string>

#include "app/cli.hpp"
#include "protocol/dto.hpp"

namespace dagent::app {

class RunOutput {
public:
    /// @brief 一轮结束后的公开结果；字段供 text/json 收尾与退出码使用。
    struct Summary {
        std::string status = "done";
        std::string error;
        std::string result;
        int steps = 0;
        int tool_calls = 0;
        protocol::Usage usage;
        bool output_failed = false;
    };

    RunOutput(OutputFormat format, std::ostream& out, std::ostream& err);

    /// @brief 记录 json 最终结果使用的会话身份。
    void set_session(std::string session_id);

    /// @brief 处理一个协议事件；返回 false 表示 jsonl stdout 已失败（调用方取消本轮）。
    bool handle(const protocol::Event& event);

    /// @brief 等模型心跳：text/json 每 interval 一行到 stderr。
    void heartbeat();

    /// @brief 写出 text/json 的最终结果并返回该轮摘要；jsonl 直接返回当前摘要。
    Summary finish(std::int64_t duration_ms);

private:
    void accumulate(const protocol::Event& event);
    void progress(const protocol::Event& event);
    bool write_line(nlohmann::json line);
    void write_stderr(const std::string& line);

    OutputFormat format_;
    std::ostream& out_;
    std::ostream& err_;
    mutable std::mutex mutex_;
    std::string session_id_;

    // 当前步骤累积与等待模型状态。
    std::string step_text_;
    bool step_had_call_ = false;
    bool waiting_ = false;
    std::chrono::steady_clock::time_point step_begin_{};

    // 结果字段在 turn_ended 时确定。
    std::string status_{"done"};
    std::string error_;
    int steps_ = 0;
    int tool_calls_ = 0;
    protocol::Usage usage_;
    bool output_failed_ = false;
};

} // namespace dagent::app
