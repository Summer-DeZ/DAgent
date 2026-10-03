#include "agent/model_budget.hpp"

namespace dagent::agent {
namespace {

std::size_t completion_tokens(const Reply& reply) {
    const auto& message = reply.message;
    std::size_t tokens = estimate_tokens(message.content) + estimate_tokens(message.reasoning_content);
    for (const auto& call : message.tool_calls)
        tokens += estimate_tokens(call.name) + estimate_tokens(call.arguments);
    return tokens;
}

} // namespace

Reply BudgetedModel::complete(const Request& request,
                             const std::function<void(const StreamEvent&)>& on_event,
                             const std::function<void(const RetryInfo&)>& on_retry,
                             std::stop_token stop, const ModelAttemptHooks& attempts) {
    bool pending = false;
    std::size_t estimated = 0;
    Reply observed;
    const auto settle = [&](const Reply& reply) {
        if (!pending) return;
        pending = false;
        const auto usage = reply.usage ? reply.usage : observed.usage;
        if (usage) {
            run_.add_usage(*usage);
            estimator_.observe_prompt_tokens(usage->prompt);
        } else {
            run_.charge_unreported_tokens(estimated + completion_tokens(reply));
        }
    };
    ModelAttemptHooks budget;
    budget.before = [&](Request& current) {
        if (attempts.before) attempts.before(current);
        if (run_.model_budget_exhausted(limits_))
            throw ModelError(ModelError::Kind::budget_exhausted, {}, "model call or token budget reached for this turn");
        estimated = estimator_.estimate(current);
        if (!run_.fit_request(current, estimated, limits_))
            throw ModelError(ModelError::Kind::budget_exhausted, {}, "model token budget reached for this turn");
        run_.count_step();
        observed = {};
        pending = true;
    };
    budget.after = [&](const Reply& reply) {
        settle(reply);
        if (attempts.after) attempts.after(reply);
    };
    try {
        return model_.complete(request, [&](const StreamEvent& event) {
            // Preserve reported usage and emitted output if the provider or a callback throws.
            if (const auto* usage = std::get_if<Usage>(&event)) observed.usage = *usage;
            else if (const auto* text = std::get_if<TextDelta>(&event)) observed.message.content += text->text;
            else if (const auto* reasoning = std::get_if<ReasoningDelta>(&event)) observed.message.reasoning_content += reasoning->text;
            else if (const auto* call = std::get_if<ToolCallBegin>(&event)) observed.message.content += call->name;
            else if (const auto* arguments = std::get_if<ToolCallDelta>(&event)) observed.message.content += arguments->args_fragment;
            on_event(event);
        }, on_retry, stop, budget);
    } catch (...) {
        settle(observed);
        throw;
    }
}

} // namespace dagent::agent
