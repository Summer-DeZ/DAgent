#include "runtime/runtime.hpp"

#include <utility>

#include "runtime/subagent.hpp"

namespace dagent::runtime {
Runtime::Runtime(Deps deps)
    : configuration_(std::move(deps.configuration)), factory_(std::move(deps.factory)),
      interactive_(deps.interactive), frontend_(deps.frontend) {
    subagent_ = std::make_unique<SubagentExecutor>(*factory_);
    broker_.set_outlet(this);
    worker_ = std::jthread([this](std::stop_token stop) { worker(stop); });
}

Runtime::~Runtime() { shutdown(); }

bool Runtime::answer(const std::string& interaction_id, agent::Decision decision) {
    return broker_.answer_approval(interaction_id, std::move(decision));
}

bool Runtime::answer(const std::string& interaction_id, agent::Answer answer) {
    return broker_.answer_question(interaction_id, std::move(answer));
}

void Runtime::interaction_requested(const InteractionRequest& request) {
    if (frontend_) frontend_->interaction_requested(request);
}

void Runtime::interaction_closed(const std::string& interaction_id) {
    if (frontend_) frontend_->interaction_closed(interaction_id);
}

} // namespace dagent::runtime
