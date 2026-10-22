#include "runtime/runtime.hpp"
#include "runtime/snapshot_projection.hpp"
#include "runtime/subagent.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

#include "agent/loop/run.hpp"
#include "agent/loop/turn_runner.hpp"
#include "base/log.hpp"
#include "base/text.hpp"

namespace dagent::runtime {
namespace {

std::shared_ptr<spdlog::logger> log_runtime() { return base::logger("runtime"); }

struct RunScope {
    agent::Session& session;
    ~RunScope() { session.end_run(); }
};

} // namespace

bool Runtime::start(const StartOptions& options) {
    // 初始化后的完整状态通过快照和历史查询交付。
    const agent::Sink initial_replay = [](const agent::Event&) {};

    std::unique_ptr<SessionInstance> instance;
    bool resumed = false;
    if (options.resume_id || options.continue_last) {
        const std::optional<std::string_view> prefix =
            options.resume_id ? std::optional<std::string_view>(*options.resume_id) : std::nullopt;
        const std::string id = factory_->resolve_session(prefix);
        instance = factory_->resume(id, std::nullopt, initial_replay);
        resumed = true;
    } else {
        instance = factory_->create_new(std::nullopt);
    }

    std::shared_ptr<SessionInstance> shared(std::move(instance));
    {
        const std::lock_guard lock(mutex_);
        current_ = shared;
        generation_ = 1;
        state_ = State::ready;
        // 启动即以 unrestricted：装配层已降为 workspace 并记录待确认目标；此处只承接 pending。
        if (const auto pending = shared->session().config().pending_permission) {
            pending_permission_ = *pending;
            pending_permission_entry_ = "startup";
        }
    }
    refresh_snapshot(*shared);
    log_runtime()->info("会话已就绪：id={} resumed={}", shared->session().meta().id, resumed);
    return resumed;
}

void Runtime::shutdown() {
    std::shared_ptr<RunControl> control;
    {
        const std::lock_guard lock(mutex_);
        if (state_ == State::closed) return;
        closed_ = true;
        state_ = State::closing;
        commands_.clear();
        queue_.clear();
        if (run_) control = run_->control;
    }
    if (control) control->stop.request_stop();
    cv_.notify_all();
    if (interactive_) broker_.cancel_all();
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
    {
        const std::lock_guard lock(mutex_);
        current_.reset();
        run_.reset();
        state_ = State::closed;
    }
}

RuntimeSnapshot Runtime::snapshot() const {
    RuntimeSnapshot snapshot;
    {
        const std::lock_guard lock(publish_mutex_);
        snapshot = snapshot_;
    }
    snapshot.mcp = mcp_states();
    return snapshot;
}

std::vector<agent::McpServerState> Runtime::mcp_states() const {
    std::shared_ptr<SessionInstance> instance;
    {
        const std::lock_guard lock(mutex_);
        instance = current_;
    }
    return instance ? instance->mcp_states() : std::vector<agent::McpServerState>{};
}

std::string Runtime::next_input_id() { return std::format("in-{}", ++input_seq_); }

std::string Runtime::next_run_id() { return std::format("run-{}", ++run_seq_); }

void Runtime::refresh_snapshot(SessionInstance& instance) {
    RuntimeSnapshot next = detail::snapshot_from_session(instance);

    {
        const std::lock_guard lock(mutex_);
        write_control_snapshot_locked(next);
        next.pending_permission_confirmation = pending_permission_;
        next.pending_permission_entry = pending_permission_entry_;
    }
    const std::lock_guard lock(publish_mutex_);
    snapshot_ = std::move(next);
}

void Runtime::sync_control_snapshot() {
    const std::lock_guard lock(mutex_);
    const std::lock_guard publish(publish_mutex_);
    write_control_snapshot_locked(snapshot_);
}

void Runtime::write_control_snapshot_locked(RuntimeSnapshot& target) const {
    target.queue.clear();
    target.operation.clear();
    target.run_id.clear();
    target.generation = generation_;
    target.busy = state_ == State::executing || state_ == State::replacing;
    for (const QueueEntry& entry : queue_) target.queue.push_back(entry.input);
    if (run_) {
        target.operation = run_->operation;
        target.run_id = run_->id;
    }
}

void Runtime::publish(EventPayload payload, const std::string& session_id,
                                std::uint64_t generation) {
    // ToolStarted 之前可能刚写入会话授权；同步一次授权列表，界面无需等下一次整体刷新。
    if (const auto* core = std::get_if<agent::Event>(&payload);
        core != nullptr && std::holds_alternative<agent::ToolStarted>(*core)) {
        std::shared_ptr<SessionInstance> instance;
        {
            const std::lock_guard lock(mutex_);
            instance = current_;
        }
        if (instance != nullptr) {
            std::vector<agent::Policy::SessionGrant> grants =
                instance->session().policy().session_grants();
            const std::lock_guard lock(publish_mutex_);
            snapshot_.grants = std::move(grants);
        }
    }
    Event event;
    {
        const std::lock_guard lock(publish_mutex_);
        event.session_id = session_id;
        event.generation = generation;
        event.payload = std::move(payload);
        detail::apply_snapshot_event(snapshot_, event);
        if (frontend_) frontend_->event(event);
    }
}

void Runtime::publish_control(ControlEvent event) {
    std::string session_id;
    std::uint64_t generation = 0;
    {
        const std::lock_guard lock(mutex_);
        if (current_) session_id = current_->session().meta().id;
        generation = generation_;
    }
    publish(EventPayload{std::move(event)}, session_id, generation);
}

void Runtime::finalize_execution() {
    std::shared_ptr<SessionInstance> instance;
    {
        const std::lock_guard lock(mutex_);
        if (state_ != State::executing) return;
        state_ = State::ready;
        run_.reset();
        instance = current_;
    }
    if (instance) refresh_snapshot(*instance);
}

std::expected<std::string, RuntimeError> Runtime::submit(
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

std::optional<QueuedInput> Runtime::recall_last() {
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

std::expected<void, RuntimeError> Runtime::enqueue(Command command, std::function<void()> accepted) {
    command.ready = !accepted;
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
    if (accepted) {
        accepted(); // 接受响应先入发送队列；此前命令不能出队。
        const std::lock_guard lock(mutex_);
        if (!commands_.empty()) commands_.front().ready = true;
    }
    cv_.notify_all();
    return {};
}

std::expected<void, RuntimeError> Runtime::new_session(CommandDone done) {
    Command command;
    command.kind = Command::Kind::new_session;
    command.done = std::move(done);
    return enqueue(std::move(command));
}

std::expected<void, RuntimeError> Runtime::resume(std::string session_id, CommandDone done) {
    Command command;
    command.kind = Command::Kind::resume;
    command.value = std::move(session_id);
    command.done = std::move(done);
    return enqueue(std::move(command));
}

std::expected<void, RuntimeError> Runtime::select_model(std::string name, CommandDone done) {
    Command command;
    command.kind = Command::Kind::select_model;
    command.value = std::move(name);
    command.done = std::move(done);
    return enqueue(std::move(command));
}

std::expected<void, RuntimeError> Runtime::add_model(agent::ModelInput input, ModelAdded done) {
    Command command;
    command.kind = Command::Kind::add_model;
    command.model = std::move(input);
    command.model_done = std::move(done);
    return enqueue(std::move(command));
}

std::expected<void, RuntimeError> Runtime::compact(std::function<void()> accepted) {
    Command command;
    command.kind = Command::Kind::compact;
    return enqueue(std::move(command), std::move(accepted));
}

std::expected<void, RuntimeError> Runtime::revoke_grant(std::string grant_id, GrantRevoked done) {
    // 撤销走即时路径：不排在 turn 之后；核对并终止使用旧授权的活跃执行。
    std::shared_ptr<SessionInstance> instance;
    {
        const std::lock_guard lock(mutex_);
        if (closed_ || state_ == State::closing || state_ == State::closed || current_ == nullptr)
            return std::unexpected(RuntimeError{RuntimeError::Kind::invalid_state, "the session is not available"});
        if (state_ == State::replacing || command_pending_)
            return std::unexpected(RuntimeError{RuntimeError::Kind::busy, "the session is busy"});
        instance = current_;
    }
    agent::Session& session = instance->session();
    const bool removed = session.policy().revoke(grant_id);
    if (removed) {
        session.committer().commit_permission_revoked(grant_id);
        session.committer().sync();
        const auto stopped = session.executions().reconcile(session.policy());
        for (const auto& [call_id, reason] : stopped) {
            log_runtime()->info("撤销 {} 终止执行 {}：{}", grant_id, call_id, reason);
        }
        // 撤销外部工具会话许可：作废该 server 的连接与旧 lease。
        if (grant_id.starts_with("external-")) {
            const std::string tool = grant_id.substr(9);
            if (tool.starts_with("mcp__")) {
                const std::size_t end = tool.find("__", 5);
                if (end != std::string::npos) instance->revoke_mcp(tool.substr(5, end - 5));
            }
        }
        // 只更新授权列表：完整快照会读执行线程正在写的对话/计划。
        std::vector<agent::Policy::SessionGrant> grants = session.policy().session_grants();
        {
            const std::lock_guard lock(publish_mutex_);
            snapshot_.grants = std::move(grants);
        }
        log_runtime()->info("撤销会话授权 {}", grant_id);
    }
    if (done) done(removed);
    return {};
}

std::expected<void, RuntimeError> Runtime::cycle_permission() {
    std::shared_ptr<SessionInstance> instance;
    std::optional<agent::PermissionMode> pending;
    agent::PermissionMode current = agent::PermissionMode::workspace;
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
        pending = pending_permission_;
        current = instance->session().policy().mode();
    }
    // pending 已存在时再次 cycle：取消待确认并跳到 unrestricted 之后的目标（ask）。
    if (pending) {
        const auto result = request_permission(agent::PermissionMode::ask, false);
        if (!result) return std::unexpected(result.error());
        return {};
    }
    agent::PermissionMode target = agent::PermissionMode::ask;
    if (!instance->session().policy().workspace_mode_available()) {
        if (current != agent::PermissionMode::ask) target = agent::PermissionMode::ask;
        else target = agent::PermissionMode::unrestricted;
    } else {
        switch (current) {
        case agent::PermissionMode::ask: target = agent::PermissionMode::workspace; break;
        case agent::PermissionMode::workspace: target = agent::PermissionMode::unrestricted; break;
        case agent::PermissionMode::unrestricted: target = agent::PermissionMode::ask; break;
        }
    }
    const auto result = request_permission(target, false);
    if (!result) return std::unexpected(result.error());
    return {};
}

std::expected<PermissionChangeResult, RuntimeError> Runtime::request_permission(
    agent::PermissionMode target, bool confirmed) {
    return request_permission_impl(target, confirmed, "cycle");
}

std::expected<PermissionChangeResult, RuntimeError> Runtime::cancel_permission_change() {
    std::shared_ptr<SessionInstance> instance;
    {
        const std::lock_guard lock(mutex_);
        if (closed_ || state_ == State::replacing || state_ == State::closing || state_ == State::closed)
            return std::unexpected(
                RuntimeError{RuntimeError::Kind::invalid_state, "the session is not available"});
        if (current_ == nullptr)
            return std::unexpected(RuntimeError{RuntimeError::Kind::invalid_state, "no current session"});
        instance = current_;
        pending_permission_.reset();
        pending_permission_entry_.clear();
    }
    const agent::PermissionMode effective = instance->session().policy().mode();
    {
        const std::lock_guard lock(publish_mutex_);
        snapshot_.permission_mode = effective;
        snapshot_.pending_permission_confirmation.reset();
        snapshot_.pending_permission_entry.clear();
    }
    // 取消后排队输入可以继续执行。
    cv_.notify_all();
    return PermissionChangeResult{effective, std::nullopt};
}

std::expected<PermissionChangeResult, RuntimeError> Runtime::request_permission_impl(
    agent::PermissionMode target, bool confirmed, std::string entry) {
    std::shared_ptr<SessionInstance> instance;
    std::uint64_t generation = 0;
    bool acked = false;
    {
        const std::lock_guard lock(mutex_);
        if (closed_ || state_ == State::replacing || state_ == State::closing || state_ == State::closed)
            return std::unexpected(
                RuntimeError{RuntimeError::Kind::invalid_state, "the session is not available"});
        if (current_ == nullptr)
            return std::unexpected(RuntimeError{RuntimeError::Kind::invalid_state, "no current session"});
        instance = current_;
        generation = generation_;
        acked = instance->session().unrestricted_acked();
    }
    agent::Session& session = instance->session();
    agent::Policy& policy = session.policy();
    target = agent::supported_mode(target, policy.workspace_mode_available());

    // 目标为 unrestricted 且尚未确认、本会话也未确认过：只记录 pending，当前权限不变。
    if (target == agent::PermissionMode::unrestricted && !confirmed && !acked) {
        const agent::PermissionMode effective = policy.mode();
        {
            const std::lock_guard lock(mutex_);
            pending_permission_ = target;
            pending_permission_entry_ = entry;
        }
        {
            const std::lock_guard lock(publish_mutex_);
            snapshot_.permission_mode = effective;
            snapshot_.pending_permission_confirmation = target;
            snapshot_.pending_permission_entry = entry;
        }
        return PermissionChangeResult{effective, target};
    }

    // 应用目标：确认、本会话已确认，或非 unrestricted 目标。
    const bool changed = policy.mode() != target;
    policy.set_mode(target);
    if (target == agent::PermissionMode::unrestricted && confirmed && interactive_)
        session.mark_unrestricted_acked(); // 只有 interactive 才算用户确认；headless 不写 ack。
    {
        const std::lock_guard lock(mutex_);
        pending_permission_.reset();
        pending_permission_entry_.clear();
    }
    if (changed) apply_permission_change(*instance);
    {
        const std::lock_guard lock(publish_mutex_);
        snapshot_.permission_mode = target;
        snapshot_.pending_permission_confirmation.reset();
        snapshot_.pending_permission_entry.clear();
    }
    if (changed) {
        publish(EventPayload{agent::ModeChanged{std::string(agent::to_string(target)), policy.planning()}},
                session.meta().id, generation);
    }
    // pending 解除后允许排队输入开始执行。
    cv_.notify_all();
    return PermissionChangeResult{target, std::nullopt};
}

std::expected<void, RuntimeError> Runtime::toggle_planning() {
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
    apply_permission_change(*instance);
    {
        const std::lock_guard lock(publish_mutex_);
        snapshot_.planning = planning;
        snapshot_.read_only = session.policy().read_only();
    }
    publish(EventPayload{agent::ModeChanged{std::string(agent::to_string(session.policy().mode())), planning}},
            session.meta().id, generation);
    return {};
}

std::expected<std::string, RuntimeError> Runtime::resolve_session(
    std::optional<std::string_view> prefix) {
    try {
        return factory_->resolve_session(prefix);
    } catch (const std::exception& error) {
        return std::unexpected(RuntimeError{RuntimeError::Kind::not_found, error.what()});
    }
}

bool Runtime::cancel(std::string_view run_id) {
    std::shared_ptr<RunControl> control;
    {
        const std::lock_guard lock(mutex_);
        if (!run_id.empty() && run_ && run_->id == run_id) control = run_->control;
    }
    if (!control) return false;
    control->stop.request_stop();
    return true;
}

void Runtime::install(PendingReplace pending, bool replace_transcript) {
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
        // 切换/新建会话：旧会话的待确认状态随 generation 变化作废；新会话若以 unrestricted
        // 建立（尚未确认）则由装配层降为 workspace 并带出待确认目标。
        pending_permission_.reset();
        pending_permission_entry_.clear();
        if (const auto pending_permission = instance->session().config().pending_permission) {
            pending_permission_ = *pending_permission;
            pending_permission_entry_ = "startup";
        }
    }
    refresh_snapshot(*instance);
    ControlEvent replaced;
    replaced.kind = ControlEvent::Kind::session_replaced;
    replaced.replace_transcript = replace_transcript;
    replaced.resumed = pending.resumed;
    publish(EventPayload{replaced}, session_id, generation);
    for (agent::Event& event : pending.replay) {
        publish(EventPayload{std::move(event)}, session_id, generation);
    }
}

Runtime::PendingReplace Runtime::prepare_new() {
    runtime::SessionState state;
    {
        const std::lock_guard lock(mutex_);
        state.mode = current_->session().policy().mode();
        state.planning = false;
        state.read_only = current_->session().config().read_only;
        state.model = current_->session().config().provider.name;
    }
    PendingReplace pending;
    pending.instance = factory_->create_new(state);
    return pending;
}

Runtime::PendingReplace Runtime::prepare_resume(const std::string& session_id) {
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
    pending.instance = factory_->resume(session_id, state, [&](const agent::Event& event) {
        pending.replay.push_back(event);
    });
    return pending;
}

Runtime::PendingReplace Runtime::prepare_switch(const std::string& model_name) {
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
    pending.instance = factory_->prepare_switch_model(
        model_name, std::move(lease), state, [&](const agent::Event& event) {
            pending.replay.push_back(event);
        });
    return pending;
}

void Runtime::apply_permission_change(SessionInstance& instance) {
    agent::Policy& policy = instance.session().policy();
    if (subagent_ != nullptr)
        subagent_->apply_parent_permission(policy.mode(), policy.read_only(), policy.planning());
    for (const auto& [call_id, reason] : instance.session().executions().reconcile(policy)) {
        log_runtime()->info("权限变化终止执行 {}：{}", call_id, reason);
    }
    // 生效权限进入只读/规划上限：外部工具被拒绝，已有连接与 lease 一并作废。
    const agent::EffectivePermission live = policy.effective();
    if (live.read_only || live.planning) instance.revoke_mcp({});
}

void Runtime::execute_command(const Command& command) {
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
            begin_execution_locked("compact");
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
            install(prepare_switch(command.value), false);
        } else if (command.kind == Command::Kind::add_model) {
            if (configuration_ == nullptr) throw std::runtime_error("model configuration is not available");
            saved_model = configuration_->add_model(command.model);
            install(prepare_switch(saved_model->name), false);
        }
        installed = true;
    } catch (const std::exception& error) {
        {
            const std::lock_guard lock(mutex_);
            state_ = State::ready;
            run_.reset();
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

void Runtime::run_input(const QueuedInput& input) {
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

    std::optional<agent::TurnEnded> ended;
    const agent::Sink sink = [this, session_id, generation, &ended](const agent::Event& event) {
        if (const auto* result = std::get_if<agent::TurnEnded>(&event)) {
            ended = *result;
            return;
        }
        publish(EventPayload{event}, session_id, generation);
    };
    const agent::Approver approver =
        !interactive_
            ? agent::Approver{}
            : agent::Approver([this, session_id, generation](const agent::Approval& approval,
                                                             std::stop_token stop) {
                  return broker_.request_approval(approval, session_id, generation, stop);
              });
    const agent::Asker asker =
        !interactive_
            ? agent::Asker{}
            : agent::Asker([this, session_id, generation](const agent::Question& question,
                                                          std::stop_token stop) {
                  return broker_.request_answer(question, session_id, generation, stop);
              });

    // 本轮的自动审阅开关：新建的每个用户请求一轮，所有子 Agent 共用。
    auto review_gate = std::make_shared<agent::RunReviewGate>();
    agent::RunServices services{sink, approver, asker, subagent_.get(), &instance->resources(),
                                run.control->stop.get_token(), std::move(review_gate)};
    agent::Run agent_run;
    agent::Session& session = instance->session();
    agent::TurnRunner runner;
    {
        const RunScope scope{session};
        try {
            session.begin_run(services, agent_run);
            runner.run(session, agent_run, services, input.text);
        } catch (const std::exception& error) {
            log_runtime()->error("回合执行发生异常：{}", error.what());
            runner.finish(session, agent_run, services, agent::TurnStatus::failed,
                          base::to_valid_utf8(error.what()));
        } catch (...) {
            log_runtime()->error("回合执行发生未知异常");
            runner.finish(session, agent_run, services, agent::TurnStatus::failed,
                          "unknown exception during turn");
        }
    }
    finalize_execution();
    if (ended) publish(EventPayload{agent::Event{*ended}}, session_id, generation);
}

void Runtime::run_compact() {
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
    agent::RunServices services{sink, approver, asker, subagent_.get(), &instance->resources(),
                                run.control->stop.get_token(), {}};
    agent::Run agent_run;
    agent::Session& session = instance->session();
    agent::TurnRunner runner;
    agent::RunOutcome outcome;
    {
        const RunScope scope{session};
        session.begin_run(services, agent_run);
        outcome = runner.compact(session, agent_run, services);
    }
    {
        const std::lock_guard lock(mutex_);
        if (state_ == State::executing) {
            state_ = State::ready;
            run_.reset();
        }
    }
    refresh_snapshot(*instance);
    ControlEvent finished;
    finished.kind = ControlEvent::Kind::operation_finished;
    finished.operation = "compact";
    finished.status = outcome.status;
    publish(EventPayload{finished}, session_id, generation);
}

void Runtime::begin_execution_locked(std::string operation) {
    state_ = State::executing;
    run_ = CurrentRun{next_run_id(), std::move(operation), std::make_shared<RunControl>()};
}

void Runtime::take_input_locked(std::optional<QueuedInput>& destination) {
    destination = std::move(queue_.front().input);
    queue_.pop_front();
    begin_execution_locked("turn");
}

void Runtime::drain() {
    for (;;) {
        std::optional<QueuedInput> next;
        {
            const std::lock_guard lock(mutex_);
            if (closed_ || state_ != State::ready || pending_permission_ || !front_ready_locked()) return;
            take_input_locked(next);
        }
        sync_control_snapshot();
        run_input(*next);
    }
}

void Runtime::worker(std::stop_token stop) {
    for (;;) {
        std::optional<Command> command;
        std::optional<QueuedInput> input;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, stop, [&] {
                return closed_ || (!commands_.empty() && commands_.front().ready) ||
                       (state_ == State::ready && !pending_permission_ && front_ready_locked());
            });
            if (closed_ || stop.stop_requested()) break;
            if (!commands_.empty() && commands_.front().ready) {
                command = std::move(commands_.front());
                commands_.pop_front();
            } else {
                take_input_locked(input);
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
            }
            sync_control_snapshot();
            ControlEvent failed;
            failed.kind = ControlEvent::Kind::failed;
            failed.operation = command ? "operation" : "turn";
            failed.error = error.what();
            publish_control(std::move(failed));
            if (input) {
                // 回合中途异常时补发失败的 TurnEnded，前端（包括 headless run）据此结束本回合。
                std::string session_id;
                std::uint64_t generation = 0;
                {
                    const std::lock_guard lock(mutex_);
                    if (current_) session_id = current_->session().meta().id;
                    generation = generation_;
                }
                publish(EventPayload{agent::Event{agent::TurnEnded{agent::TurnStatus::failed, error.what(), 0, 0, {}}}},
                        session_id, generation);
            }
        } catch (...) {
            log_runtime()->error("执行线程发生未知异常");
            {
                const std::lock_guard lock(mutex_);
                state_ = State::ready;
                run_.reset();
            }
            sync_control_snapshot();
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
