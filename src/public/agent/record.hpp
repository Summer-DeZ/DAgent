/// @file record.hpp
/// @brief 核心往会话里写什么：记录类型和 payload、写入顺序、写入失败时降级。
///
/// 存储格式、脱敏、blob、截断恢复都由 session 模块负责（session 文档）。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/events.hpp"
#include "agent/model.hpp"
#include "agent/message.hpp"
#include "lib/nlohmann/json.hpp"
#include "session/session.hpp"
#include "tools/tools.hpp"

namespace dagent::agent {

/// @brief 会话写入器。写入失败时进入停用状态（broken），之后的记录全部丢弃，本轮继续。
class Recorder {
public:
    static Recorder create(const session::Options&, session::Meta);
    static Recorder resume(const session::Options&, std::string_view id);

    void system(std::string_view text);
    void user(std::int64_t n, std::string_view text);
    void assistant(std::int64_t n, const Reply&);
    void tool(std::int64_t n, const ToolCall&, std::string_view summary, const tools::Result&);
    void permission(std::string_view call_id, const Decision&, std::string_view rule);
    void turn_end(TurnStatus, std::string_view error, int steps, int tool_calls, const Usage& total);
    void turn_end_crashed();
    void sync();

    const session::Meta& meta() const;
    bool broken() const { return broken_; }
    const std::string& error() const { return error_; } ///< broken 时的原因，给 Notice 用

private:
    explicit Recorder(session::Writer writer) : writer_(std::move(writer)) {}

    void append(std::string_view type, nlohmann::json payload);

    std::optional<session::Writer> writer_;
    bool broken_ = false;
    std::string error_;
};

} // namespace dagent::agent
