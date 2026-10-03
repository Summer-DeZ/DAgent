/// @file record_codec.hpp
/// @brief RecordCodec：记录 payload 的唯一编解码入口。
///
/// 编码输出采用 events.type/payload 格式；解码产生类型化事实并校验当前字段形状，集中处理
/// 错误分类（corrupt）。恢复与历史投影共用本解码器，不各自手写字段解析。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agent/events.hpp"
#include "agent/message.hpp"
#include "agent/port_journal.hpp"
#include "agent/reply.hpp"
#include "agent/tool_data.hpp"
#include "lib/nlohmann/json.hpp"

namespace dagent::agent::record_codec {

// ---- 编码 ----

Record system(std::string_view text, std::string_view model);
Record user(std::int64_t n, std::string_view text, const std::vector<SkillView>& skills = {});
Record assistant(std::int64_t n, const Reply& reply);
Record tool_started(const ToolStarted& event);
Record tool(std::int64_t n, const ToolCall& call, std::string_view summary, const ToolResult& result);
Record permission(const Approval& approval, const Decision& decision,
                  const ExecutionGrant* grant = nullptr);
Record parent_review(const Approval& approval, const Decision& decision);
std::string permission_summary(const Approval& approval, const Decision& decision);
Record permission_revoked(std::string_view id);
Record prune(const std::vector<std::int64_t>& ordinals);
Record compaction(std::int64_t keep_from, std::string_view summary);
Record turn_end(TurnStatus status, std::string_view error, int steps, int tool_calls, const Usage& total);
Record turn_end_crashed();

// ---- 解码：类型化事实 ----

struct SystemRecord {
    std::string text;
    std::string model;
};

struct UserRecord {
    std::int64_t n = 0;
    std::string text;
    std::vector<SkillView> skills; ///< explicit requests only; never restored as active guidance
};

struct AssistantRecord {
    std::int64_t n = 0;
    Message message;
    std::string finish;
    std::optional<Usage> usage;
};

struct ToolStartedRecord {
    ToolStarted event;
};

struct ToolRecord {
    std::int64_t n = 0;
    ToolCall call;
    std::string summary;
    ToolResult result;
};

struct PermissionRecord {
    std::string call_id, answer, rule;
    ApprovalAuthority authority = ApprovalAuthority::user;
    ApprovalIdentity identity;
    std::string state, scope, reason, model, tool;
    Usage usage;
    nlohmann::json audit;
    std::string summary;
};

struct ParentReviewRecord { PermissionRecord review; };

struct PermissionRevokedRecord {
    std::string id;
};

struct PruneRecord {
    std::vector<std::int64_t> ordinals;
};

struct CompactionRecord {
    std::int64_t keep_from = -1;
    std::string summary;
};

struct TurnEndRecord {
    TurnStatus status = TurnStatus::done;
    std::string error;
    int steps = 0, tool_calls = 0;
    Usage usage;
};

using DecodedRecord =
    std::variant<SystemRecord, UserRecord, AssistantRecord, ToolStartedRecord, ToolRecord,
                 PermissionRecord, ParentReviewRecord, PermissionRevokedRecord, PruneRecord,
                 CompactionRecord, TurnEndRecord>;

/// @brief 解码一条记录；未知类型或非法必须字段抛 RecordError(corrupt)。
DecodedRecord decode(std::string_view type, const nlohmann::json& payload);

} // namespace dagent::agent::record_codec
