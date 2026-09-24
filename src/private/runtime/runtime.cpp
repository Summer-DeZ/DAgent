#include "runtime/runtime.hpp"

#include <utility>

#include "runtime/subagent.hpp"

namespace dagent::runtime {
Runtime::Runtime(Deps deps)
    : configuration_(std::move(deps.configuration)), factory_(std::move(deps.factory)),
      frontend_(deps.frontend) {
    subagent_ = std::make_unique<SubagentExecutor>(*factory_);
    broker_.set_outlet(this);
    SessionController::Deps controller;
    controller.factory = factory_.get();
    controller.configuration = configuration_.get();
    controller.broker = deps.interactive ? &broker_ : nullptr;
    controller.delegation = subagent_.get();
    controller.sink = [this](const Event& event) {
        if (frontend_) frontend_->event(event);
    };
    controller_ = std::make_unique<SessionController>(std::move(controller));
}

Runtime::~Runtime() { shutdown(); }

StartResult Runtime::start(const StartOptions& options) { return controller_->start(options); }

RuntimeSnapshot Runtime::snapshot() const {
    RuntimeSnapshot snapshot = controller_->snapshot();
    snapshot.mcp = controller_->mcp_states();
    return snapshot;
}

std::expected<std::string, RuntimeError> Runtime::submit(
    std::string text, std::function<void(const std::string&)> accepted) {
    return controller_->submit(std::move(text), std::move(accepted));
}

std::optional<QueuedInput> Runtime::recall_last() { return controller_->recall_last(); }

std::expected<void, RuntimeError> Runtime::new_session(CommandDone done) {
    return controller_->new_session(std::move(done));
}

std::expected<void, RuntimeError> Runtime::resume(std::string session_id, CommandDone done) {
    return controller_->resume(std::move(session_id), std::move(done));
}

std::expected<void, RuntimeError> Runtime::select_model(std::string name, CommandDone done) {
    return controller_->select_model(std::move(name), std::move(done));
}

std::expected<void, RuntimeError> Runtime::add_model(agent::ModelInput input, ModelAdded done) {
    return controller_->add_model(std::move(input), std::move(done));
}

std::expected<void, RuntimeError> Runtime::compact() { return controller_->compact(); }

std::expected<bool, RuntimeError> Runtime::revoke_grant(const std::string& grant_id) {
    return controller_->revoke_grant(grant_id);
}

std::expected<void, RuntimeError> Runtime::cycle_permission() {
    return controller_->cycle_permission();
}

std::expected<void, RuntimeError> Runtime::toggle_planning() {
    return controller_->toggle_planning();
}

bool Runtime::cancel(std::string_view run_id) { return controller_->cancel(run_id); }

std::expected<std::string, RuntimeError> Runtime::resolve_session(
    std::optional<std::string_view> prefix) {
    return controller_->resolve_session(prefix);
}

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

void Runtime::shutdown() {
    if (controller_ != nullptr) controller_->shutdown();
}

} // namespace dagent::runtime
