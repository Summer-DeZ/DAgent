/// @file record.hpp
/// @brief 核心往会话里写什么：记录类型和 payload、写入顺序、写入失败时降级。
///
/// 存储格式、脱敏、blob、截断恢复都由 session 模块负责（session 文档）。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/events.hpp"
#include "agent/conversation.hpp"
#include "agent/reply.hpp"
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

    void system(std::string_view text, std::string_view model);
    void user(std::int64_t n, std::string_view text);
    void assistant(std::int64_t n, const Reply&);
    void tool_started(const ToolStarted&);
    void tool(std::int64_t n, const ToolCall&, std::string_view summary, const tools::Result&);
    void permission(const Approval&, const Decision&);
    void permission_revoked(std::string_view id);
    void prune(const std::vector<std::int64_t>& ordinals);
    void compaction(std::int64_t keep_from, std::string_view summary);
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

struct Restored {
    std::string model; ///< 最近的 system 记录，旧格式回落到会话 Meta
    Conversation conversation;
    bool unfinished = false;          ///< 最后一轮没有 turn_end（崩溃或被杀）
    std::vector<ToolCall> open_calls; ///< unfinished 时还没有结果的调用
};

/// @brief 从记录重建消息历史，并把可显示的历史事件交给 sink。
Restored replay_into(const session::Options&, std::string_view id, const Sink& sink);

/// @brief session::list 的标题提取器：第一条 user 的第一行，最多 60 个 UTF-8 字符。
std::string session_title(const nlohmann::json& first_events);

/// @brief 在当前项目的会话中解析完整 id 或唯一前缀；prefix 为空时选择最近会话。
std::string resolve_session_id(const session::Options&, const std::filesystem::path& cwd,
                               std::optional<std::string_view> prefix);

} // namespace dagent::agent
