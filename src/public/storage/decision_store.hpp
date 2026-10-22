/// @file decision_store.hpp
/// @brief 决策审阅存储（Home/data 下独立 SQLite）：审阅记录、人工反馈与动态阈值。
///
/// 与会话数据库分开；缺失或损坏不能阻断会话恢复。写入失败由调用方退回后续审批。
#pragma once

#include <filesystem>
#include <memory>
#include <optional>

#include "agent/port/decision.hpp"

namespace dagent::storage {

struct DecisionStoreOptions {
    std::filesystem::path database; ///< 例如 <root>/data/decision.sqlite
    std::optional<agent::ThresholdProfile> initial_profile; ///< 仅补齐缺失键，不覆盖已有动态值。
};

/// @brief 校验阈值数值、方向与上下界；非法快照抛 StorageError。
void validate_decision_profile(const agent::ThresholdProfile&);

/// @brief 打开（必要时创建）决策审阅库；打不开时抛 StorageError。
std::shared_ptr<agent::DecisionReviewStore> open_decision_store(const DecisionStoreOptions&);

} // namespace dagent::storage
