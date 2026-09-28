/// @file dto.hpp
/// @brief 私有协议的纯数据 DTO：会话快照、事件信封、历史条目与交互请求。
///
/// protocol 只依赖 base/nlohmann：不包含核心实现类型，工具/展示数据保持 JSON 值；
/// 核心值 → DTO 的转换集中在 backend adapter。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lib/nlohmann/json.hpp"

namespace dagent::protocol {

/// 业务协议版本（与 JSON-RPC 的 jsonrpc="2.0" 分开）。
inline constexpr int kMajor = 1;
inline constexpr int kMinor = 1;

struct PublicModel {
    std::string name, kind, model, base_url;
    std::size_t max_tokens = 0;
    double temperature = -1.0;
    std::size_t context_window = 0;
    bool has_key = false;
};

/// @brief provider 种类的公开元数据（模型表单校验与默认值；不含配置内容）。
struct ProviderKind {
    std::string kind, default_base_url;
    bool needs_credential = false;
};

struct Usage {
    std::int64_t prompt = 0, completion = 0, cached = 0;
};

struct QueueItem {
    std::string input_id;
    std::string text_preview;
};

struct OperationInfo {
    std::string kind; ///< turn / compact / replacing
    std::string run_id; ///< 无运行时为空
};

struct ContextInfo {
    std::size_t used = 0, limit = 0;
    int trigger_percent = 0; ///< 配置中的压缩触发百分比
    std::size_t window = 0; ///< 配置的窗口预算；模型表单默认值用
};

struct RecordingInfo {
    bool broken = false;
    std::string error;
};

/// @brief session.* 的权威只读状态；字段与运行时快照一一对应。
struct SessionSnapshot {
    std::string session_id;
    std::uint64_t session_generation = 0;
    std::uint64_t state_seq = 0;
    PublicModel model;
    std::string permission_mode;
    bool planning = false, read_only = false, busy = false;
    std::optional<OperationInfo> current_operation;
    std::vector<QueueItem> queue;
    ContextInfo context;
    nlohmann::json work_plan = nlohmann::json::array();
    nlohmann::json mcp = nlohmann::json::array();
    RecordingInfo recording;
};

/// @brief 历史条目；一个持久记录产生 0/1 条。
struct HistoryItem {
    std::string kind; ///< user / assistant / tool / tool_started / system / turn_end
    std::int64_t seq = -1;
    std::string text, reasoning, finish, model;
    std::string call_id, name, summary;
    nlohmann::json result;  ///< tool：ToolResult JSON（model_text/is_error/interrupted/display）
    nlohmann::json started; ///< tool_started：事件 JSON
    std::string status, error;
    int steps = 0, tool_calls = 0;
    nlohmann::json usage;
};

/// @brief event 通知的固定信封（协议 §5）。
struct Event {
    std::uint64_t seq = 0;
    std::string session_id;
    std::uint64_t session_generation = 0;
    std::string kind;
    nlohmann::json data = nlohmann::json::object();
    std::optional<std::string> parent_session_id;
    std::optional<std::string> model_call_id;
    std::optional<std::string> agent;
};

/// @brief interaction.requested / closed 的载荷。
struct InteractionRequest {
    std::string interaction_id;
    std::string kind; ///< approval / question
    std::string session_id;
    std::uint64_t session_generation = 0;
    nlohmann::json payload; ///< approval 或 question 的 DTO
};

/// @brief 业务错误 data.kind 的固定取值（协议 §8）。
namespace error_kind {
inline constexpr const char* kBusy = "busy";
inline constexpr const char* kStaleSession = "stale_session";
inline constexpr const char* kNotFound = "not_found";
inline constexpr const char* kSessionInUse = "session_in_use";
inline constexpr const char* kInteractionClosed = "interaction_closed";
inline constexpr const char* kInvalidState = "invalid_state";
inline constexpr const char* kConfigError = "config_error";
inline constexpr const char* kQueryFailed = "query_failed";
inline constexpr const char* kStartupFailed = "startup_failed";
inline constexpr const char* kVersionMismatch = "version_mismatch";
inline constexpr const char* kClosing = "closing";
} // namespace error_kind

// ---------------------------------------------------------------- JSON 转换

void to_json(nlohmann::json&, const PublicModel&);
void from_json(const nlohmann::json&, PublicModel&);
void to_json(nlohmann::json&, const ProviderKind&);
void from_json(const nlohmann::json&, ProviderKind&);
void to_json(nlohmann::json&, const Usage&);
void from_json(const nlohmann::json&, Usage&);
void to_json(nlohmann::json&, const QueueItem&);
void from_json(const nlohmann::json&, QueueItem&);
void to_json(nlohmann::json&, const OperationInfo&);
void from_json(const nlohmann::json&, OperationInfo&);
void to_json(nlohmann::json&, const ContextInfo&);
void from_json(const nlohmann::json&, ContextInfo&);
void to_json(nlohmann::json&, const RecordingInfo&);
void from_json(const nlohmann::json&, RecordingInfo&);
void to_json(nlohmann::json&, const SessionSnapshot&);
void from_json(const nlohmann::json&, SessionSnapshot&);
void to_json(nlohmann::json&, const HistoryItem&);
void from_json(const nlohmann::json&, HistoryItem&);
void to_json(nlohmann::json&, const Event&);
void from_json(const nlohmann::json&, Event&);
void to_json(nlohmann::json&, const InteractionRequest&);
void from_json(const nlohmann::json&, InteractionRequest&);

} // namespace dagent::protocol
