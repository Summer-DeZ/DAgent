/// @file decision.hpp
/// @brief 决策审阅的两个外部端口：DecisionModel（推理）与 DecisionReviewStore（记录与阈值）。
///
/// 核心只调用 evaluate() 与存储端口；HTTP、协议、SQLite 都留在外围实现。
/// 未配置决策模型时两个指针都为空，保持原有审批行为。
#pragma once

#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>

#include "agent/decision_types.hpp"

namespace dagent::agent {

/// @brief 决策模型端口：一次请求 → 一次概率响应。
///
/// 实现负责排队、编码、解码与取消；不授予权限、不写会话记录。
/// 超时从提交请求起计入排队与 HTTP 等待；deadline 与 stop 均可中断。
class DecisionModel {
public:
    virtual ~DecisionModel() = default;
    virtual std::expected<DecisionReply, DecisionError> evaluate(const DecisionRequest&,
                                                                 ReviewDeadline,
                                                                 std::stop_token) = 0;
};

/// @brief 审阅记录与阈值端口：Home/data 下的独立审阅数据，与会话记录分开。
class DecisionReviewStore {
public:
    virtual ~DecisionReviewStore() = default;

    /// @brief 取该身份版本的阈值快照；没有对应配置时返回 nullopt。
    virtual std::optional<ThresholdProfile> thresholds(const ReviewVersion&) = 0;
    /// @brief 追加一条审阅记录；写入失败不生成无法追溯的批准。
    virtual void append(const DecisionReviewRecord&) = 0;
    /// @brief 人工最终决定回填；同一 approval_request_id 只处理一次，并更新动态阈值。
    virtual void finish(const ReviewFeedback&) = 0;
};

/// @brief 行为配置：超时与阈值配置名（对应 config.json 的 decision_review 段）。
struct DecisionReviewOptions {
    std::chrono::milliseconds timeout{10000};
    std::string threshold_profile = "default";
};

/// @brief 一个后端实例共享的决策审阅依赖；Session 与子会话共用同一快照。
struct DecisionServices {
    std::shared_ptr<DecisionModel> model;
    std::shared_ptr<DecisionReviewStore> store;
    DecisionReviewOptions options;
    ReviewVersion version;

    bool enabled() const { return model != nullptr; }
};

} // namespace dagent::agent
