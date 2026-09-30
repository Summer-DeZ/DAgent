#include "agent/session.hpp"

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
    return RequestShape{config_.system_prompt, catalog_.specs(),
                        run_ ? run_->skills().context() : std::string{}, model_params()};
}

Request Session::build_request() const {
    const RequestShape shape = request_shape();
    return conversation_.build(shape.system, shape.tools, shape.params, shape.turn_context);
}

ToolResult Session::activate_skill(std::string_view name) {
    ToolResult result;
    const auto* skill = config_.skills ? config_.skills->find(name) : nullptr;
    if (!skill || !run_ || !catalog_.contains("skill")) {
        result.is_error = true;
        result.model_text = "Skill is unavailable: " + std::string(name);
        return result;
    }
    result.display = SkillView{skill->name, skill->file.string()};
    if (run_->skills().contains(name)) {
        result.model_text = "Skill already active for this turn: " + skill->name;
        return result;
    }
    RunSkills candidate = run_->skills();
    candidate.activate(*skill);
    // Activation can happen while an assistant tool batch is open. Estimate only the
    // irreducible request here; the normal compactor handles older history next step.
    Conversation minimum;
    std::string input;
    for (auto it = conversation_.entries().rbegin(); it != conversation_.entries().rend(); ++it) {
        if (it->message.role == Role::user) { input = it->message.content; break; }
    }
    minimum.add_user(std::move(input));
    if (estimator_.estimate(minimum.build(config_.system_prompt, catalog_.specs(), model_params(),
                                           candidate.context())) > compactor_.budget().limit) {
        result.is_error = true;
        result.model_text = "Skill instructions exceed the context budget: " + skill->name +
                            ". Select fewer skills or use a larger model window.";
        return result;
    }
    run_->skills() = std::move(candidate);
    result.model_text = "Loaded skill for this turn: " + skill->name +
                        ". Full instructions are in the Active skills block on the latest user message.";
    return result;
}

std::size_t Session::estimated_tokens() {
    const std::size_t value = estimator_.estimate(build_request());
    estimated_tokens_.store(value);
    return value;
}

void Session::begin_run(const RunServices& services, Run& run) {
    run_ = &run;
    committer_.set_sink(services.sink);
    control_.begin_turn(ControlActionExecutor::Services{&services.asker, &services.approver,
                                                        services.delegation, &policy_, config_.read_only,
                                                        services.sink, meta_.id, config_.provider.name, catalog_.names(),
                                                        [this](std::string_view name) { return activate_skill(name); }});
}

SessionSnapshot Session::snapshot() {
    SessionSnapshot snapshot;
    snapshot.permission_mode = policy_.mode();
    snapshot.read_only = policy_.read_only();
    snapshot.planning = policy_.planning();
    snapshot.plan = plan_.view();
    snapshot.grants = policy_.session_grants();
    snapshot.used_tokens = estimated_tokens_.load();
    snapshot.token_limit = compactor_.budget().limit;
    return snapshot;
}

} // namespace dagent::agent
