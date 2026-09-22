#include "runtime/controller.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

#include "agent/run.hpp"
#include "agent/turn_runner.hpp"
#include "base/log.hpp"

namespace dagent::runtime {
namespace {

std::shared_ptr<spdlog::logger> log_runtime() { return base::logger("runtime"); }

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

} // namespace

SessionController::SessionController(Deps deps) : deps_(std::move(deps)) {
    worker_ = std::jthread([this](std::stop_token stop) { worker(stop); });
}

SessionController::~SessionController() { shutdown(); }

StartResult SessionController::start(const StartOptions& options) {
    std::vector<agent::Event> replay;
    const agent::Sink collect = [&replay](const agent::Event& event) { replay.push_back(event); };

    std::unique_ptr<SessionInstance> instance;
    bool resumed = false;
    if (options.resume_id || options.continue_last) {
        const std::optional<std::string_view> prefix =
            options.resume_id ? std::optional<std::string_view>(*options.resume_id) : std::nullopt;
        const std::string id = deps_.factory->resolve_session(prefix);
        instance = deps_.factory->resume(id, std::nullopt, collect);
        resumed = true;
    } else {
        instance = deps_.factory->create_new(std::nullopt, collect);
    }

    std::shared_ptr<SessionInstance> shared(std::move(instance));
    {
        const std::lock_guard lock(mutex_);
        current_ = shared;
        generation_ = 1;
        state_ = State::ready;
    }
    refresh_snapshot(*shared);
    log_runtime()->info("会话已就绪：id={} resumed={}", shared->session().meta().id, resumed);
    return StartResult{resumed, std::move(replay)};
}

void SessionController::shutdown() {
    std::shared_ptr<RunControl> control;
    {
        const std::lock_guard lock(mutex_);
        if (state_ == State::closed) return;
        closed_ = true;
        state_ = State::closing;
        commands_.clear();
        queue_.clear();
        control = control_;
    }
    if (control) control->stop.request_stop();
    cv_.notify_all();
    if (deps_.broker != nullptr) deps_.broker->cancel_all();
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
    {
        const std::lock_guard lock(mutex_);
        current_.reset();
        run_.reset();
        control_.reset();
        state_ = State::closed;
    }
}

RuntimeSnapshot SessionController::snapshot() const {
    const std::lock_guard lock(publish_mutex_);
    return snapshot_;
}

std::vector<agent::McpServerState> SessionController::mcp_states() const {
    std::shared_ptr<SessionInstance> instance;
    {
        const std::lock_guard lock(mutex_);
        instance = current_;
    }
    return instance ? instance->mcp_states() : std::vector<agent::McpServerState>{};
}

std::string SessionController::next_input_id() { return std::format("in-{}", ++input_seq_); }

std::string SessionController::next_run_id() { return std::format("run-{}", ++run_seq_); }

void SessionController::refresh_snapshot(SessionInstance& instance) {
    agent::Session& session = instance.session();
    RuntimeSnapshot next;
    next.session_id = session.meta().id;
    next.model = session.config().provider;
    next.trigger_percent = session.config().options.context.compaction_trigger_percent;
    next.window_tokens = session.config().options.context.window_tokens;
    next.cwd = session.config().cwd;
    next.project_root = session.config().project_root;
    const agent::SessionSnapshot values = session.snapshot(); // 执行线程上的即时值
    next.permission_mode = values.permission_mode;
    next.planning = values.planning;
    next.read_only = values.read_only;
    next.used_tokens = values.used_tokens;
    next.token_limit = values.token_limit;
    next.work_plan = values.plan;
    next.grants = values.grants;
    next.recording_broken = session.committer().broken();
    next.recording_error = session.committer().error();

    {
        const std::lock_guard lock(mutex_);
        next.generation = generation_;
        next.busy = state_ == State::executing || state_ == State::replacing;
        for (const QueueEntry& entry : queue_) next.queue.push_back(entry.input);
        if (run_) {
            next.operation = run_->operation;
            next.run_id = run_->id;
        }
    }
    const std::lock_guard lock(publish_mutex_);
    next.state_seq = event_seq_;
    snapshot_ = std::move(next);
}

void SessionController::sync_control_snapshot() {
    const std::lock_guard lock(mutex_);
    const std::lock_guard publish(publish_mutex_);
    snapshot_.generation = generation_;
    snapshot_.busy = state_ == State::executing || state_ == State::replacing;
    snapshot_.queue.clear();
    for (const QueueEntry& entry : queue_) snapshot_.queue.push_back(entry.input);
    snapshot_.operation.clear();
    snapshot_.run_id.clear();
    if (run_) {
        snapshot_.operation = run_->operation;
        snapshot_.run_id = run_->id;
    }
}

void SessionController::publish(EventPayload payload, const std::string& session_id,
                                std::uint64_t generation) {
    Event event;
    {
        const std::lock_guard lock(publish_mutex_);
        event.seq = ++event_seq_;
        event.session_id = session_id;
        event.generation = generation;
        event.payload = std::move(payload);
        snapshot_.state_seq = event.seq;
        apply_to_snapshot(event);
        if (deps_.sink) deps_.sink(event);
    }
}

void SessionController::publish_control(ControlEvent event) {
    std::string session_id;
    std::uint64_t generation = 0;
    {
        const std::lock_guard lock(mutex_);
        if (current_) session_id = current_->session().meta().id;
        generation = generation_;
    }
    publish(EventPayload{std::move(event)}, session_id, generation);
}

void SessionController::apply_to_snapshot(const Event& event) {
    std::visit(Overloaded{
                   [&](const agent::ContextUpdate& update) {
                       snapshot_.used_tokens = update.used;
                       snapshot_.token_limit = update.limit;
                   },
                   [&](const agent::ModeChanged& mode) {
                       snapshot_.planning = mode.planning;
                       snapshot_.permission_mode =
                           mode.mode == "ask" ? agent::PermissionMode::ask
                           : mode.mode == "unrestricted" ? agent::PermissionMode::unrestricted
                                                         : agent::PermissionMode::workspace;
                       if (mode.planning) snapshot_.read_only = true;
                   },
                   [&](const agent::ModelChanged& model) { snapshot_.model.model = model.model; },
                   [&](const agent::ToolFinished& finished) {
                       if (const auto* plan = std::get_if<agent::TodoView>(&finished.result.display))
                           snapshot_.work_plan = *plan;
                   },
                   [&](const auto&) {},
               },
               event.payload);
}

void SessionController::finalize_execution() {
    std::shared_ptr<SessionInstance> instance;
    {
        const std::lock_guard lock(mutex_);
        if (state_ != State::executing) return;
        state_ = State::ready;
        run_.reset();
        control_.reset();
        instance = current_;
    }
    if (instance) refresh_snapshot(*instance);
}

std::expected<std::string, RuntimeError> SessionController::submit(
    std::string text, std::function<void(const std::string&)> accepted) {
    std::string id;
    bool wake = false;
    {
        const std::lock_guard lock(mutex_);
        if (closed_ || state_ == State::closing || state_ == State::closed) {
            return std::unexpected(
                RuntimeError{RuntimeError::Kind::closing, "the backend is shutting down"});
        }
        if (current_ == nullptr) {
            return std::unexpected(RuntimeError{RuntimeError::Kind::invalid_state, "no current session"});
        }
        if (state_ == State::replacing) {
            return std::unexpected(RuntimeError{RuntimeError::Kind::busy, "the session is being replaced"});
        }
        if (command_pending_) {
            return std::unexpected(RuntimeError{RuntimeError::Kind::busy, "the session is busy"});
        }
        id = next_input_id();
        queue_.push_back(QueueEntry{QueuedInput{id, std::move(text)}, false});
        wake = state_ == State::ready;
    }
    if (accepted) accepted(id); // 接受响应先入发送队列；此前 Run 不会开始（ready=false）
    {
        const std::lock_guard lock(mutex_);
        for (QueueEntry& entry : queue_) {
            if (entry.input.id == id) entry.ready = true;
        }
    }
    sync_control_snapshot();
    if (wake) cv_.notify_all();
    return id;
}

std::optional<QueuedInput> SessionController::recall_last() {
    std::optional<QueuedInput> recalled;
    {
        const std::lock_guard lock(mutex_);
        if (closed_ || queue_.empty()) return std::nullopt;
        // 只取回已确认接受的输入；刚提交、响应尚未入队的条目跳过。
        auto it = queue_.end();
        while (it != queue_.begin()) {
            --it;
            if (it->ready) break;
        }
        if (it == queue_.end() || !it->ready) return std::nullopt;
        recalled = std::move(it->input);
        queue_.erase(it);
    }
    sync_control_snapshot();
    return recalled;
}

std::expected<void, RuntimeError> SessionController::enqueue(Command command) {
    {
        const std::lock_guard lock(mutex_);
        if (closed_ || state_ == State::closing || state_ == State::closed) {
            return std::unexpected(
                RuntimeError{RuntimeError::Kind::closing, "the backend is shutting down"});
        }
        if (current_ == nullptr) {
            return std::unexpected(RuntimeError{RuntimeError::Kind::invalid_state, "no current session"});
        }
        if (state_ != State::ready || command_pending_ || !queue_.empty()) {
            return std::unexpected(RuntimeError{RuntimeError::Kind::busy, "the session is busy"});
        }
        commands_.push_back(std::move(command));
        command_pending_ = true;
    }
    cv_.notify_all();
    return {};
}

std::expected<void, RuntimeError> SessionController::new_session(CommandDone done) {
    Command command;
    command.kind = Command::Kind::new_session;
    command.done = std::move(done);
    return enqueue(std::move(command));
}

std::expected<void, RuntimeError> SessionController::resume(std::string session_id, CommandDone done) {
    Command command;
    command.kind = Command::Kind::resume;
    command.value = std::move(session_id);
    command.done = std::move(done);
    return enqueue(std::move(command));
}

std::expected<void, RuntimeError> SessionController::select_model(std::string name, CommandDone done) {
    Command command;
    command.kind = Command::Kind::select_model;
    command.value = std::move(name);
    command.done = std::move(done);
    return enqueue(std::move(command));
}

std::expected<void, RuntimeError> SessionController::add_model(agent::ModelInput input, ModelAdded done) {
    Command command;
    command.kind = Command::Kind::add_model;
    command.model = std::move(input);
    command.model_done = std::move(done);
    return enqueue(std::move(command));
}

std::expected<void, RuntimeError> SessionController::compact() {
    Command command;
    command.kind = Command::Kind::compact;
    return enqueue(std::move(command));
}

std::expected<bool, RuntimeError> SessionController::revoke_grant(const std::string& grant_id) {
    std::shared_ptr<SessionInstance> instance;
    {
        const std::lock_guard lock(mutex_);
        if (closed_) {
            return std::unexpected(
                RuntimeError{RuntimeError::Kind::closing, "the backend is shutting down"});
        }
        if (state_ != State::ready || current_ == nullptr) {
            return std::unexpected(RuntimeError{RuntimeError::Kind::busy, "the session is busy"});
        }
        instance = current_;
    }
    agent::Session& session = instance->session();
    if (!session.policy().revoke(grant_id)) return false;
    session.committer().commit_permission_revoked(grant_id);
    session.committer().sync();
    refresh_snapshot(*instance);
    return true;
}

std::expected<void, RuntimeError> SessionController::cycle_permission() {
    std::shared_ptr<SessionInstance> instance;
    std::uint64_t generation = 0;
    {
        const std::lock_guard lock(mutex_);
        if (closed_ || command_pending_ || state_ == State::replacing ||
            state_ == State::closing || state_ == State::closed) {
            return std::unexpected(
                RuntimeError{RuntimeError::Kind::invalid_state, "the session is not available"});
        }
        if (current_ == nullptr) {
            return std::unexpected(RuntimeError{RuntimeError::Kind::invalid_state, "no current session"});
        }
        instance = current_;
        generation = generation_;
    }
    agent::Policy& policy = instance->session().policy();
    switch (policy.mode()) {
    case agent::PermissionMode::ask: policy.set_mode(agent::PermissionMode::workspace); break;
    case agent::PermissionMode::workspace: policy.set_mode(agent::PermissionMode::unrestricted); break;
    case agent::PermissionMode::unrestricted: policy.set_mode(agent::PermissionMode::ask); break;
    }
    {
        const std::lock_guard lock(publish_mutex_);
        snapshot_.permission_mode = policy.mode();
    }
    publish(EventPayload{agent::ModeChanged{std::string(agent::to_string(policy.mode())), policy.planning()}},
            instance->session().meta().id, generation);
    return {};
}

std::expected<void, RuntimeError> SessionController::toggle_planning() {
    std::shared_ptr<SessionInstance> instance;
    std::uint64_t generation = 0;
    {
        const std::lock_guard lock(mutex_);
        if (closed_ || state_ == State::replacing || state_ == State::closing || state_ == State::closed) {
            return std::unexpected(
                RuntimeError{RuntimeError::Kind::invalid_state, "the session is not available"});
        }
        if (state_ != State::ready || command_pending_ || current_ == nullptr) {
            return std::unexpected(RuntimeError{RuntimeError::Kind::busy, "the session is busy"});
        }
        instance = current_;
        generation = generation_;
    }
    agent::Session& session = instance->session();
    const bool planning = !session.policy().planning();
    session.policy().set_planning(planning);
    session.policy().set_read_only(planning || session.config().read_only);
    {
        const std::lock_guard lock(publish_mutex_);
        snapshot_.planning = planning;
        snapshot_.read_only = session.policy().read_only();
    }
    publish(EventPayload{agent::ModeChanged{std::string(agent::to_string(session.policy().mode())), planning}},
            session.meta().id, generation);
    return {};
}

std::expected<std::string, RuntimeError> SessionController::resolve_session(
    std::optional<std::string_view> prefix) {
    try {
        return deps_.factory->resolve_session(prefix);
    } catch (const std::exception& error) {
        return std::unexpected(RuntimeError{RuntimeError::Kind::not_found, error.what()});
    }
}

void SessionController::cancel() {
    std::shared_ptr<RunControl> control;
    {
        const std::lock_guard lock(mutex_);
        control = control_;
    }
    if (control) control->stop.request_stop();
}

bool SessionController::cancel(std::string_view run_id) {
    std::shared_ptr<RunControl> control;
    {
        const std::lock_guard lock(mutex_);
        if (!run_id.empty() && run_ && run_->id == run_id) control = control_;
    }
    if (!control) return false;
    control->stop.request_stop();
    return true;
}

void SessionController::install(PendingReplace pending, bool replace_transcript) {
    std::shared_ptr<SessionInstance> instance(std::move(pending.instance));
    const std::string session_id = instance->session().meta().id;
    std::uint64_t generation = 0;
    {
        const std::lock_guard lock(mutex_);
        current_ = instance;
        generation_ += 1;
        generation = generation_;
        state_ = State::ready;
        run_.reset();
        control_.reset();
    }
    refresh_snapshot(*instance);
    ControlEvent replaced;
    replaced.kind = ControlEvent::Kind::session_replaced;
    replaced.replace_transcript = replace_transcript;
    replaced.resumed = pending.resumed;
    publish(EventPayload{replaced}, session_id, generation);
    if (pending.added_model) {
        ControlEvent models;
        models.kind = ControlEvent::Kind::models_changed;
        publish(EventPayload{std::move(models)}, session_id, generation);
    }
    for (agent::Event& event : pending.replay) {
        publish(EventPayload{std::move(event)}, session_id, generation);
    }
}

SessionController::PendingReplace SessionController::prepare_new() {
    runtime::SessionState state;
    {
        const std::lock_guard lock(mutex_);
        state.mode = current_->session().policy().mode();
        state.planning = false;
        state.read_only = current_->session().config().read_only;
        state.model = current_->session().config().provider.name;
    }
    PendingReplace pending;
    pending.instance = deps_.factory->create_new(state, [&](const agent::Event& event) {
        pending.replay.push_back(event);
    });
    return pending;
}

SessionController::PendingReplace SessionController::prepare_resume(const std::string& session_id) {
    PendingReplace pending;
    pending.resumed = true;
    runtime::SessionState state;
    {
        const std::lock_guard lock(mutex_);
        state.mode = current_->session().policy().mode();
        state.planning = current_->session().policy().planning();
        state.read_only = current_->session().config().read_only;
        state.model = current_->session().config().provider.name;
    }
    pending.instance = deps_.factory->resume(session_id, state, [&](const agent::Event& event) {
        pending.replay.push_back(event);
    });
    return pending;
}

SessionController::PendingReplace SessionController::prepare_switch(const std::string& model_name,
                                                                   bool added_model) {
    std::shared_ptr<agent::SessionLease> lease;
    runtime::SessionState state;
    {
        const std::lock_guard lock(mutex_);
        lease = current_->lease();
        state.mode = current_->session().policy().mode();
        state.planning = current_->session().policy().planning();
        state.read_only = current_->session().config().read_only;
        state.model = current_->session().config().provider.name;
    }
    PendingReplace pending;
    pending.added_model = added_model;
    pending.instance = deps_.factory->prepare_switch_model(
        model_name, std::move(lease), state, [&](const agent::Event& event) {
            pending.replay.push_back(event);
        });
    return pending;
}

void SessionController::execute_command(const Command& command) {
    const auto operation_name = [&]() -> std::string {
        switch (command.kind) {
        case Command::Kind::new_session: return "new session";
        case Command::Kind::resume: return "resume";
        case Command::Kind::select_model: return "switch model";
        case Command::Kind::add_model: return "add model";
        case Command::Kind::compact: return "compact";
        }
        return "operation";
    };

    if (command.kind == Command::Kind::compact) {
        {
            const std::lock_guard lock(mutex_);
            if (state_ != State::ready) return;
            state_ = State::executing;
            auto control = std::make_shared<RunControl>();
            run_ = CurrentRun{next_run_id(), "compact", control};
            control_ = std::move(control);
        }
        sync_control_snapshot();
        run_compact();
        drain();
        return;
    }

    {
        const std::lock_guard lock(mutex_);
        if (state_ != State::ready) return;
        state_ = State::replacing;
        run_ = CurrentRun{"", "replacing", nullptr};
        control_.reset();
    }
    sync_control_snapshot();

    std::optional<agent::PublicModel> saved_model;
    bool installed = false;
    try {
        if (command.kind == Command::Kind::new_session) {
            install(prepare_new(), true);
        } else if (command.kind == Command::Kind::resume) {
            install(prepare_resume(command.value), true);
        } else if (command.kind == Command::Kind::select_model) {
            install(prepare_switch(command.value, false), false);
        } else if (command.kind == Command::Kind::add_model) {
            if (deps_.configuration == nullptr) throw std::runtime_error("model configuration is not available");
            saved_model = deps_.configuration->add_model(command.model);
            install(prepare_switch(saved_model->name, true), false);
        }
        installed = true;
    } catch (const std::exception& error) {
        {
            const std::lock_guard lock(mutex_);
            state_ = State::ready;
            run_.reset();
            control_.reset();
        }
        sync_control_snapshot();
        ControlEvent failed;
        failed.kind = ControlEvent::Kind::failed;
        failed.operation = operation_name();
        failed.error = error.what();
        log_runtime()->warn("{} 失败：{}", operation_name(), error.what());
        publish_control(std::move(failed));
        if (command.done) {
            command.done(std::unexpected(RuntimeError{RuntimeError::Kind::invalid_state, error.what()}));
        }
        if (command.model_done && command.kind == Command::Kind::add_model) {
            command.model_done(saved_model.value_or(agent::PublicModel{}), error.what());
        }
    }
    if (installed) {
        if (command.done) command.done({});
        if (command.model_done && saved_model) command.model_done(*saved_model, {});
    }
    drain();
}

void SessionController::run_input(const QueuedInput& input) {
    std::shared_ptr<SessionInstance> instance;
    CurrentRun run;
    std::uint64_t generation = 0;
    {
        const std::lock_guard lock(mutex_);
        instance = current_;
        if (instance == nullptr || !run_) return;
        run = *run_;
        generation = generation_;
    }
    const std::string session_id = instance->session().meta().id;

    const agent::Sink sink = [this, session_id, generation](const agent::Event& event) {
        if (std::holds_alternative<agent::TurnEnded>(event)) finalize_execution();
        publish(EventPayload{event}, session_id, generation);
    };
    const agent::Approver approver = [this, session_id, generation](const agent::Approval& approval,
                                                                   std::stop_token stop) {
        if (deps_.broker == nullptr) return agent::Decision{agent::Decision::Answer::deny, {}, false};
        return deps_.broker->request_approval(approval, session_id, generation, stop);
    };
    const agent::Asker asker = [this, session_id, generation](const agent::Question& question,
                                                             std::stop_token stop) {
        if (deps_.broker == nullptr) return agent::Answer{{}, {}, true};
        return deps_.broker->request_answer(question, session_id, generation, stop);
    };

    agent::RunServices services{sink, approver, asker, deps_.delegation, &instance->resources(),
                                run.control->stop.get_token()};
    agent::Run agent_run(agent::RunKind::turn, run.id);
    agent_run.begin(run.control->stop.get_token());
    agent::Session& session = instance->session();
    session.begin_run(services, agent_run);
    agent::TurnRunner runner;
    runner.run(session, agent_run, services, input.text);
    session.end_run();
    finalize_execution();
}

void SessionController::run_compact() {
    std::shared_ptr<SessionInstance> instance;
    CurrentRun run;
    std::uint64_t generation = 0;
    {
        const std::lock_guard lock(mutex_);
        instance = current_;
        if (instance == nullptr || !run_) return;
        run = *run_;
        generation = generation_;
    }
    const std::string session_id = instance->session().meta().id;
    const agent::Sink sink = [this, session_id, generation](const agent::Event& event) {
        publish(EventPayload{event}, session_id, generation);
    };
    const agent::Approver approver{};
    const agent::Asker asker{};
    agent::RunServices services{sink, approver, asker, deps_.delegation, &instance->resources(),
                                run.control->stop.get_token()};
    agent::Run agent_run(agent::RunKind::compact, run.id);
    agent_run.begin(run.control->stop.get_token());
    agent::Session& session = instance->session();
    session.begin_run(services, agent_run);
    agent::TurnRunner runner;
    const agent::RunOutcome outcome = runner.compact(session, agent_run, services);
    session.end_run();
    {
        const std::lock_guard lock(mutex_);
        if (state_ == State::executing) {
            state_ = State::ready;
            run_.reset();
            control_.reset();
        }
    }
    refresh_snapshot(*instance);
    ControlEvent finished;
    finished.kind = ControlEvent::Kind::operation_finished;
    finished.operation = "compact";
    finished.status = outcome.status;
    publish(EventPayload{finished}, session_id, generation);
}

void SessionController::drain() {
    for (;;) {
        std::optional<QueuedInput> next;
        {
            const std::lock_guard lock(mutex_);
            if (closed_ || state_ != State::ready || !front_ready_locked()) return;
            next = std::move(queue_.front().input);
            queue_.pop_front();
            state_ = State::executing;
            auto control = std::make_shared<RunControl>();
            run_ = CurrentRun{next_run_id(), "turn", control};
            control_ = std::move(control);
        }
        sync_control_snapshot();
        run_input(*next);
    }
}

void SessionController::worker(std::stop_token stop) {
    for (;;) {
        std::optional<Command> command;
        std::optional<QueuedInput> input;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, stop, [&] {
                return closed_ || !commands_.empty() || (state_ == State::ready && front_ready_locked());
            });
            if (closed_ || stop.stop_requested()) break;
            if (!commands_.empty()) {
                command = std::move(commands_.front());
                commands_.pop_front();
            } else {
                input = std::move(queue_.front().input);
                queue_.pop_front();
                state_ = State::executing;
                auto control = std::make_shared<RunControl>();
                run_ = CurrentRun{next_run_id(), "turn", control};
                control_ = std::move(control);
            }
        }
        try {
            if (command) {
                execute_command(*command);
            } else if (input) {
                sync_control_snapshot();
                run_input(*input);
                drain();
            }
        } catch (const std::exception& error) {
            log_runtime()->error("执行线程发生异常：{}", error.what());
            {
                const std::lock_guard lock(mutex_);
                state_ = State::ready;
                run_.reset();
                control_.reset();
            }
            ControlEvent failed;
            failed.kind = ControlEvent::Kind::failed;
            failed.operation = command ? "operation" : "turn";
            failed.error = error.what();
            publish_control(std::move(failed));
        } catch (...) {
            log_runtime()->error("执行线程发生未知异常");
            const std::lock_guard lock(mutex_);
            state_ = State::ready;
            run_.reset();
            control_.reset();
        }
        if (command) {
            const std::lock_guard lock(mutex_);
            command_pending_ = false;
        }
    }
    std::shared_ptr<SessionInstance> instance;
    {
        const std::lock_guard lock(mutex_);
        instance = current_;
    }
    if (instance) {
        // 收尾同步；实例所有权仍在控制器，最终由析构释放写租约。
        instance->session().committer().sync();
    }
}

} // namespace dagent::runtime
