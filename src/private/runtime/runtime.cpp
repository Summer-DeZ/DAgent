#include "runtime/runtime.hpp"

#include <utility>

#include "runtime/subagent.hpp"

namespace dagent::runtime {
namespace {

RuntimeError query_error(const std::exception& error) {
    return RuntimeError{RuntimeError::Kind::query_failed, error.what()};
}

} // namespace

Runtime::Runtime(Deps deps)
    : configuration_(std::move(deps.configuration)), queries_(std::move(deps.queries)),
      factory_(std::move(deps.factory)) {
    frontend_.store(deps.frontend);
    subagent_ = std::make_unique<SubagentExecutor>(*factory_);
    broker_.set_outlet(this);
    SessionController::Deps controller;
    controller.factory = factory_.get();
    controller.configuration = configuration_.get();
    controller.broker = deps.interactive ? &broker_ : nullptr;
    controller.delegation = subagent_.get();
    controller.sink = [this](const Event& event) {
        if (Frontend* frontend = frontend_.load()) frontend->event(event);
    };
    controller_ = std::make_unique<SessionController>(std::move(controller));
    query_thread_ = std::jthread([this](std::stop_token stop) { query_worker(stop); });
}

Runtime::~Runtime() { shutdown(); }

void Runtime::set_frontend(Frontend* frontend) { frontend_.store(frontend); }

StartResult Runtime::start(const StartOptions& options) { return controller_->start(options); }

RuntimeSnapshot Runtime::snapshot() const {
    RuntimeSnapshot snapshot = controller_->snapshot();
    snapshot.mcp = controller_->mcp_states();
    return snapshot;
}

std::vector<agent::PublicModel> Runtime::models() const { return configuration_->models(); }

std::vector<agent::ProviderKindInfo> Runtime::provider_kinds() const {
    return configuration_->provider_kinds();
}

std::filesystem::path Runtime::theme_file() const { return configuration_->theme_file(); }

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

void Runtime::cancel() { controller_->cancel(); }

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
    if (Frontend* frontend = frontend_.load()) frontend->interaction_requested(request);
}

void Runtime::interaction_closed(const std::string& interaction_id) {
    if (Frontend* frontend = frontend_.load()) frontend->interaction_closed(interaction_id);
}

void Runtime::post_query(std::function<void()> job) {
    {
        const std::lock_guard lock(query_mutex_);
        if (query_closed_) return;
        query_jobs_.push_back(std::move(job));
    }
    query_cv_.notify_all();
}

void Runtime::query_worker(std::stop_token stop) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(query_mutex_);
            query_cv_.wait(lock, stop, [&] { return query_closed_ || !query_jobs_.empty(); });
            if ((query_closed_ && query_jobs_.empty()) || stop.stop_requested()) return;
            job = std::move(query_jobs_.front());
            query_jobs_.pop_front();
        }
        job();
    }
}

void Runtime::query_sessions(std::size_t limit, std::function<void(SessionsResult)> done) {
    post_query([this, limit, done = std::move(done)] {
        try {
            done(queries_->sessions(limit));
        } catch (const std::exception& error) {
            done(std::unexpected(query_error(error)));
        }
    });
}

void Runtime::query_history(std::string session_id, std::function<void(HistoryResult)> done) {
    post_query([this, session_id = std::move(session_id), done = std::move(done)] {
        try {
            done(queries_->history(session_id));
        } catch (const std::exception& error) {
            done(std::unexpected(query_error(error)));
        }
    });
}

void Runtime::query_workspace(std::function<void(WorkspaceResult)> done) {
    post_query([this, done = std::move(done)] {
        try {
            done(queries_->workspace());
        } catch (const std::exception& error) {
            done(std::unexpected(query_error(error)));
        }
    });
}

void Runtime::query_files(std::string query, std::size_t limit, std::function<void(FilesResult)> done) {
    post_query([this, query = std::move(query), limit, done = std::move(done)] {
        try {
            done(queries_->complete(query, limit));
        } catch (const std::exception& error) {
            done(std::unexpected(query_error(error)));
        }
    });
}

void Runtime::shutdown() {
    if (controller_ != nullptr) controller_->shutdown();
    {
        const std::lock_guard lock(query_mutex_);
        query_closed_ = true;
        query_jobs_.clear();
    }
    query_cv_.notify_all();
    query_thread_.request_stop();
    if (query_thread_.joinable()) query_thread_.join();
}

} // namespace dagent::runtime
