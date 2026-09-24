/// @file output.hpp
/// @brief run 模式的旧输出适配：协议事件 → 原 text/json/jsonl 外观。
///
/// 这是记录路线 §1 的 LegacyOutputCodec：只从实时事件（出口 2）恢复公开输出，
/// 不打印 RPC 信封，也不直接使用持久 payload。字段形状与 docs/design/agent.md §12 一致。
#pragma once

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <mutex>
#include <string>

#include "app/cli.hpp"
#include "protocol/dto.hpp"

namespace dagent::app {

class LegacyOutputCodec {
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

    LegacyOutputCodec(OutputFormat format, std::ostream& out, std::ostream& err);

    /// @brief 记录会话身份；jsonl 同时写出首行 session 元信息，其他格式只用于 json 结果字段。
    /// 返回 false 表示 jsonl stdout 已失败。
    bool session_header(const std::string& session_id, bool resumed);

    /// @brief 处理一个协议事件；返回 false 表示 jsonl stdout 已失败（调用方取消本轮）。
    bool handle(const protocol::Event& event);

    /// @brief 等模型心跳：text/json 每 interval 一行到 stderr。
    void heartbeat();

    /// @brief 写出 text/json 的最终结果并返回该轮摘要；jsonl 直接返回当前摘要。
    Summary finish(std::int64_t duration_ms);

private:
    void stream_event(const protocol::Event& event);
    void jsonl_event(const protocol::Event& event);
    bool write_line(nlohmann::json line);
    void write_stderr(const std::string& line);

    OutputFormat format_;
    std::ostream& out_;
    std::ostream& err_;
    mutable std::mutex mutex_;
    std::string session_id_;

    // text/json：当前步骤累积与等待模型状态。
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
