#include "agent/agent.hpp"

#include <format>
#include <utility>

#include "agent/turn_runner.hpp"
#include "app/session_assembly.hpp"
#include "base/log.hpp"
#include "runtime/subagent.hpp"

namespace dagent::agent {

Agent::Agent(Setup setup, std::unique_ptr<app::SessionAssembly> factory,
             std::unique_ptr<runtime::SessionInstance> instance)
    : factory_(std::move(factory)), instance_(std::move(instance)) {
    subagent_ = std::make_unique<runtime::SubagentExecutor>(*factory_);
    if (setup.permission_mode == PermissionMode::unrestricted && !setup.sandbox.read_only_ready()) {
        base::logger("agent")->warn(
            "sandbox support is unavailable; unrestricted commands run with full host access");
    }
}

std::unique_ptr<Agent> Agent::create(Setup setup) {
    app::SessionAssembly::Options options;
    options.assembly = setup.assembly;
    options.base = setup;
    options.resolve_model = {};
    auto factory = std::make_unique<app::SessionAssembly>(std::move(options));
    auto instance = factory->create_new(std::nullopt, [](const Event&) {});
    return std::unique_ptr<Agent>(new Agent(std::move(setup), std::move(factory), std::move(instance)));
}

std::unique_ptr<Agent> Agent::resume(Setup setup, std::string_view session_id,
                                     const Sink& replay_sink) {
    app::SessionAssembly::Options options;
    options.assembly = setup.assembly;
    options.base = setup;
    options.resolve_model = {};
    auto factory = std::make_unique<app::SessionAssembly>(std::move(options));
    auto instance = factory->resume(session_id, std::nullopt, replay_sink);
    return std::unique_ptr<Agent>(new Agent(std::move(setup), std::move(factory), std::move(instance)));
}

Agent::~Agent() = default;

std::vector<std::string> Agent::tool_names() const { return instance_->session().tool_names(); }

TurnStatus Agent::run_turn(std::string input, const TurnContext& ctx) {
    Session& session = instance_->session();
    RunServices services{ctx.sink, ctx.approver, ctx.asker, subagent_.get(), &instance_->resources(),
                         ctx.stop};
    Run run(RunKind::turn, std::format("run-{}", ++run_seq_));
    run.begin(ctx.stop);
    session.begin_run(services, run);
    TurnRunner runner;
    const RunOutcome outcome = runner.run(session, run, services, std::move(input));
    session.end_run();
    return outcome.status;
}

TurnStatus Agent::compact(const TurnContext& ctx) {
    Session& session = instance_->session();
    RunServices services{ctx.sink, ctx.approver, ctx.asker, subagent_.get(), &instance_->resources(),
                         ctx.stop};
    Run run(RunKind::compact, std::format("run-{}", ++run_seq_));
    run.begin(ctx.stop);
    session.begin_run(services, run);
    TurnRunner runner;
    const RunOutcome outcome = runner.compact(session, run, services);
    session.end_run();
    return outcome.status;
}

void Agent::set_permission_mode(PermissionMode mode) { instance_->session().policy().set_mode(mode); }

void Agent::set_read_only(bool value) { instance_->session().policy().set_read_only(value); }

bool Agent::read_only() const { return instance_->session().policy().read_only(); }

void Agent::set_plan_mode(bool value) {
    Session& session = instance_->session();
    session.policy().set_planning(value);
    session.policy().set_read_only(value || session.config().read_only);
}

bool Agent::planning() const { return instance_->session().policy().planning(); }

PermissionMode Agent::permission_mode() const { return instance_->session().policy().mode(); }

std::vector<Policy::SessionGrant> Agent::session_grants() const {
    return instance_->session().policy().session_grants();
}

bool Agent::revoke_permission(std::string_view id) {
    Session& session = instance_->session();
    if (!session.policy().revoke(id)) return false;
    session.committer().commit_permission_revoked(id);
    session.committer().sync();
    return true;
}

const SessionMeta& Agent::meta() const { return instance_->session().meta(); }

} // namespace dagent::agent
