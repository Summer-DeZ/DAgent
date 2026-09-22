#include "agent/session.hpp"

#include <format>
#include <utility>

namespace dagent::agent {

Session::Session(SessionConfig config, SessionMeta meta, std::unique_ptr<JournalWriter> journal,
                 std::shared_ptr<ModelSession> model, ToolSession& tools, ActionCatalog::Config catalog,
                 Conversation conversation, WorkPlan plan)
    : config_(std::move(config)), meta_(std::move(meta)), journal_(std::move(journal)),
      model_(std::move(model)), tools_(tools), catalog_(tools_, std::move(catalog)),
      policy_(config_.permission_mode, config_.read_only, config_.planning, config_.sandbox,
              config_.sandbox_options, config_.shell_analysis_version, config_.cwd, config_.project_root,
              config_.control_root),
      plan_(std::move(plan)),
      conversation_(std::move(conversation)),
      compactor_(config_.options.context, config_.provider.max_tokens, config_.compact_prompt),
      committer_(conversation_, plan_, *journal_) {}

Session::~Session() = default;

ModelParams Session::model_params() const {
    return ModelParams{config_.provider.model, config_.provider.max_tokens, config_.provider.temperature};
}

RequestShape Session::request_shape() const {
    return RequestShape{config_.system_prompt, catalog_.specs(), model_params()};
}

Request Session::build_request() const {
    const RequestShape shape = request_shape();
    return conversation_.build(shape.system, shape.tools, shape.params);
}

std::size_t Session::estimated_tokens() { return estimator_.estimate(build_request()); }

std::string Session::next_invocation_id() { return std::format("inv-{}", ++invocation_seq_); }

void Session::begin_run(const RunServices& services, Run& run) {
    committer_.set_sink(services.sink);
    control_.begin_turn(ControlActionExecutor::Services{&services.asker, &services.approver,
                                                        services.delegation, &policy_, config_.read_only,
                                                        services.sink, &run, meta_.id, config_.provider.name, catalog_.names()});
}

SessionSnapshot Session::snapshot() {
    SessionSnapshot snapshot;
    snapshot.id = meta_.id;
    snapshot.model = config_.provider.model;
    snapshot.permission_mode = policy_.mode();
    snapshot.read_only = policy_.read_only();
    snapshot.planning = policy_.planning();
    snapshot.plan = plan_.view();
    snapshot.grants = policy_.session_grants();
    snapshot.used_tokens = estimated_tokens();
    snapshot.token_limit = compactor_.budget().limit;
    return snapshot;
}

} // namespace dagent::agent
