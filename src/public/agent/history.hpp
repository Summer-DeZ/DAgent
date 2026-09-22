/// @file history.hpp
/// @brief 只读历史投影：把解码后的记录变成可显示条目，并跨页校验记录顺序与配对。
///
/// HistoryProjector 不构造 Session、不调模型、不连 MCP、不写库（记录路线 §6、§7）；
/// HistoryCursor 只保存角色/ordinal/开放调用等必要元数据，不构造完整 Conversation。
#pragma once

#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <vector>

#include "agent/events.hpp"
#include "agent/port_store.hpp"
#include "agent/record_codec.hpp"
#include "agent/tool_data.hpp"

namespace dagent::agent {

/// @brief 一条历史展示条目；一个记录可以产生 0 或 1 条（记录与条目不是一一计数关系）。
struct HistoryItem {
    enum class Kind { user, assistant, tool, tool_started, system, turn_end };

    Kind kind = Kind::user;
    std::int64_t seq = -1;

    std::string text;      ///< user/assistant 正文
    std::string reasoning; ///< assistant 思考
    std::string finish;    ///< assistant 原始 finish_reason

    ToolStarted started;   ///< tool_started
    std::string call_id, name, summary; ///< tool
    ToolResult result;     ///< tool

    std::string model;     ///< system：历史模型标签

    TurnStatus status = TurnStatus::done; ///< turn_end
    std::string error;
    int steps = 0, tool_calls = 0;
    Usage usage;
};

/// @brief 记录 → 历史条目；未知显示分支不产生条目。
class HistoryProjector {
public:
    std::vector<HistoryItem> project(const StoredRecord& record) const;
    std::vector<HistoryItem> project(const StoredRecord&, const record_codec::DecodedRecord&) const;
};

/// @brief 跨页记录验证游标：ordinal 连续、工具配对、prune/compaction 目标合法。
class HistoryCursor {
public:
    /// @brief 校验一条已解码记录；违反约束抛 RecordError(corrupt)。
    void observe(const record_codec::DecodedRecord& record);

private:
    bool saw_system_ = false;
    std::int64_t next_ordinal_ = 0;
    std::deque<std::string> open_calls_; ///< 当前 assistant 批次尚未闭合的调用 id，按序
    std::set<std::int64_t> tool_ordinals_;
    std::set<std::int64_t> safe_cut_ordinals_; ///< 可作为 compaction keep_from 的条目 ordinal
    bool first_entry_ = true;
};

} // namespace dagent::agent
