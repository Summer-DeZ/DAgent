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
    /// 最近真实用户原话（最多 3 条，最新在末尾）；委派任务不计入。
    std::vector<std::string> recent_user_messages;
    /// 子会话初始化记录（主要任务与辅助用户原话）；旧会话缺失时保持为空。
    bool has_delegation = false;
    std::string delegated_task;
    std::vector<std::string> delegated_user_messages;
    /// 本会话用户已确认 unrestricted 警告；缺失（旧会话）表示未确认，不从旧 mode 反推。
    bool unrestricted_acked = false;
};

class SessionRecovery {
public:
    /// @brief 按 seq 升序的全部记录重建会话；非法历史抛 RecordError(corrupt)。
    /// child 表示子会话：旧 user 记录缺少 origin 时按会话身份判定来源。
    RecoveryResult restore(const std::vector<StoredRecord>& records, bool child = false) const;
};

} // namespace dagent::agent
