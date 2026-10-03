/// @file model_budget.hpp
/// @brief 本轮所有模型请求的共享预算，包括审阅、摘要和实际重试。
#pragma once

#include "agent/port_model.hpp"
#include "agent/run.hpp"
#include "agent/tokens.hpp"

namespace dagent::agent {

class BudgetedModel final : public ModelSession {
public:
    BudgetedModel(ModelSession& model, Run& run, const Limits& limits, TokenEstimator& estimator)
        : model_(model), run_(run), limits_(limits), estimator_(estimator) {}

    Reply complete(const Request&, const std::function<void(const StreamEvent&)>&,
                   const std::function<void(const RetryInfo&)>&, std::stop_token,
                   const ModelAttemptHooks& = {}) override;

private:
    ModelSession& model_;
    Run& run_;
    const Limits& limits_;
    TokenEstimator& estimator_;
};

} // namespace dagent::agent
