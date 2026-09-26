/// @file recovery.hpp
/// @brief SessionRecovery：把记录解码结果重建成可执行的会话状态与中断报告。
///
/// 只做纯状态重建与校验：不执行工具、不启动模型、不写库、不发显示事件。
/// 显式恢复的补闭合与 prompt 更新由调用方在取得写所有权后完成。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agent/conversation.hpp"
#include "agent/message.hpp"
#include "agent/port_store.hpp"
#include "agent/work_plan.hpp"

namespace dagent::agent {

struct RecoveryResult {
    std::string model; ///< 最近 system 记录中的模型
    Conversation conversation;
    WorkPlan plan; ///< 从 Todo View 重建的整份计划
    bool unfinished = false;          ///< 最后一轮没有 turn_end
    std::vector<ToolCall> open_calls; ///< unfinished 时尚未有结果的调用
};

class SessionRecovery {
public:
    /// @brief 按 seq 升序的全部记录重建会话；非法历史抛 RecordError(corrupt)。
    RecoveryResult restore(const std::vector<StoredRecord>& records) const;
};

} // namespace dagent::agent
