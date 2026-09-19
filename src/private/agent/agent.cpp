#include "agent/agent.hpp"

#include <cstddef>
#include <format>
#include <utility>

#include "agent/prompt.hpp"
#include "base/log.hpp"
#include "base/text.hpp"
#include "workspace/context.hpp"

namespace dagent::agent {
namespace {

std::shared_ptr<spdlog::logger> log_agent() { return base::logger("agent"); }

bool sandbox_available(const exec::Support& support) {
    return support.landlock_abi > 0 && support.seccomp;
}

std::string render_prompt(const Setup& setup) {
    const std::string_view tmpl = setup.system_prompt_override
                                      ? std::string_view(*setup.system_prompt_override)
                                      : builtin_system_prompt();
    const workspace::Environment env = workspace::collect_environment(setup.cwd);
    PromptVars vars;
    vars.model = setup.model.model;
    vars.project_root = setup.project_root;
    vars.sandbox = sandbox_available(setup.sandbox);
    vars.permission_mode = std::string(to_string(setup.permission_mode));
    return render_system_prompt(tmpl, env, vars);
}

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

} // namespace

Agent::Agent(Setup setup, std::string system_prompt, Recorder recorder, Conversation conversation)
    : setup_(std::move(setup)),
      recorder_(std::move(recorder)),
      registry_(),
      tool_ctx_(setup_.cwd, setup_.tools, setup_.files, setup_.search, setup_.process),
      policy_(setup_.permission_mode, setup_.sandbox, setup_.cwd, setup_.project_root),
      model_([codec = setup_.codec] { return make_openai_chat_codec(codec); }, setup_.http,
             RetryOptions{setup_.options.run.max_model_retries}),
      conversation_(std::move(conversation)),
      estimator_(),
      compactor_(setup_.options.context, setup_.model.max_tokens,
                 workspace::render(builtin_compact_prompt(), nlohmann::json::object())),
      system_prompt_(std::move(system_prompt)) {
    tools::add_builtin(registry_);
}

std::unique_ptr<Agent> Agent::resume(Setup setup, std::string_view session_id,
                                     const Sink& replay_sink) {
    Restored restored = replay_into(setup.session, session_id, replay_sink);
    Recorder recorder = Recorder::resume(setup.session, session_id);
    const std::string previous_model = recorder.meta().model;
    std::string system_prompt = render_prompt(setup);

    auto agent = std::unique_ptr<Agent>(new Agent(std::move(setup), std::move(system_prompt),
                                                  std::move(recorder),
                                                  std::move(restored.conversation)));
    if (restored.unfinished) {
        for (const ToolCall& call : restored.open_calls) {
            const std::string text(texts::kCrashed);
            const std::string summary = "恢复意外中断的 " + call.name;
            const std::int64_t ordinal =
                agent->conversation_.add_tool_result(call.id, text, summary);
            tools::Result result;
            result.text = text;
            result.is_error = true;
            result.interrupted = true;
            agent->recorder_.tool(ordinal, call, summary, result);
            replay_sink(ToolFinished{call.id, call.name, summary, result});
        }
        agent->recorder_.turn_end_crashed();
        agent->recorder_.sync();
        replay_sink(TurnEnded{TurnStatus::failed, "会话意外中断", 0, 0, {}});
    }

    if (const std::optional<std::string> invalid = agent->conversation_.validate()) {
        throw session::SessionError(session::SessionError::Kind::corrupt,
                                    "会话历史不一致：" + *invalid);
    }

    agent->recorder_.system(agent->system_prompt_);
    if (previous_model != agent->setup_.model.model) {
        replay_sink(Notice{Notice::Level::info,
                           std::format("这个会话原来用的是 {}，现在用 {} 继续", previous_model,
                                       agent->setup_.model.model)});
    }
    if (agent->recorder_.broken()) {
        log_agent()->error("会话记录写入失败：{}", agent->recorder_.error());
    }
    log_agent()->info("会话已恢复：id={} model={}", agent->meta().id, agent->setup_.model.model);
    replay_sink(ContextUpdate{{}, agent->estimator_.estimate(agent->conversation_.build(
                                      agent->system_prompt_, agent->tool_defs(), agent->setup_.model)),
                               agent->compactor_.budget().limit});
    return agent;
}

std::unique_ptr<Agent> Agent::create(Setup setup) {
    std::string system_prompt = render_prompt(setup);

    session::Meta meta;
    meta.id = session::new_id();
    meta.cwd = setup.cwd;
    meta.git_root = setup.git_root.value_or(std::filesystem::path{});
    meta.model = setup.model.model;
    Recorder recorder = Recorder::create(setup.session, std::move(meta));

    auto agent =
        std::unique_ptr<Agent>(new Agent(std::move(setup), std::move(system_prompt), std::move(recorder)));
    agent->recorder_.system(agent->system_prompt_);
    if (agent->recorder_.broken()) log_agent()->error("会话记录写入失败：{}", agent->recorder_.error());
    log_agent()->info("会话已创建：id={} model={}", agent->meta().id, agent->meta().model);
    return agent;
}

std::vector<ToolDef> Agent::tool_defs() const {
    std::vector<ToolDef> defs;
    for (const tools::Spec* spec : registry_.specs()) {
        defs.push_back(ToolDef{spec->name, spec->description, spec->parameters});
    }
    return defs;
}

void Agent::report_stream(const StreamEvent& event, const Sink& sink) {
    std::visit(Overloaded{
                   [&](const TextDelta& delta) { sink(TextDelta{delta.text}); },
                   [&](const ReasoningDelta& delta) { sink(ReasoningDelta{delta.text}); },
                   [&](const ToolCallBegin& begin) { sink(ToolPending{begin.id, begin.name}); },
                   [&](const auto&) {},
               },
               event);
}

void Agent::report_retry(const RetryInfo& info, const Sink& sink) {
    sink(Retrying{info.attempt, info.max_attempts, info.wait, info.reason});
    if (info.had_output) sink(StreamReset{});
}

void Agent::check_broken(const Sink& sink) {
    if (!recorder_.broken() || broken_notified_) return;
    broken_notified_ = true;
    sink(Notice{Notice::Level::error,
                std::format("会话记录写入失败，之后的内容不会保存：{}", recorder_.error())});
}

void Agent::keep_partial(const Reply& partial, const Sink& sink) {
    if (partial.message.content.empty()) return;

    Message message;
    message.role = Role::assistant;
    message.content = partial.message.content + std::string(texts::kInterrupted);
    Reply stored;
    stored.message = message;
    const std::int64_t ordinal = conversation_.add_assistant(std::move(message));
    recorder_.assistant(ordinal, stored);
    check_broken(sink);
}

TurnStatus Agent::finish(TurnStatus status, std::string error, int steps, int calls, const Usage& total,
                         const Sink& sink) {
    for (const ToolCall& call : conversation_.open_calls()) {
        const std::string text(texts::kInterruptedCall);
        const std::int64_t ordinal = conversation_.add_tool_result(call.id, text, "中断");
        tools::Result result;
        result.text = text;
        result.interrupted = true;
        recorder_.tool(ordinal, call, "中断", result);
    }
    check_broken(sink);
    recorder_.turn_end(status, error, steps, calls, total);
    recorder_.sync();
    log_agent()->info("本轮结束：status={} steps={} tool_calls={} prompt={} completion={}",
                      to_string(status), steps, calls, total.prompt, total.completion);
    sink(TurnEnded{status, error, steps, calls, total});
    return status;
}

TurnStatus Agent::run_turn(std::string input, const Sink& sink, const Approver& approver,
                           std::stop_token stop) {
    input = base::to_valid_utf8(input);
    const std::int64_t user_ordinal = conversation_.add_user(input);
    recorder_.user(user_ordinal, input);
    check_broken(sink);
    sink(TurnStarted{input});

    const int max_model_calls = setup_.options.run.max_model_calls;
    const int max_tool_calls = setup_.options.run.max_tool_calls;
    const std::size_t context_limit = compactor_.budget().limit;

    int steps = 0, calls = 0;
    bool grace = false;
    Usage total;

    const auto on_stream = [&](const StreamEvent& event) { report_stream(event, sink); };
    const auto on_retry = [&](const RetryInfo& info) { report_retry(info, sink); };

    for (;;) {
        if (steps >= max_model_calls) {
            return finish(TurnStatus::limit, "本轮模型调用次数已达上限", steps, calls, total, sink);
        }
        ++steps;

        const RequestShape shape{system_prompt_, tool_defs(), setup_.model};
        std::size_t estimated = 0;
        Reply reply;
        try {
            // 自动压缩在 StepStarted 之前（01-events §3）：界面在一步开始后作废的内容不含压缩提示。
            compactor_.maybe_compact(conversation_, shape, model_, estimator_, recorder_, sink, stop);
            check_broken(sink);
            sink(StepStarted{steps});
            for (int attempt = 0; ; ++attempt) {
                const Request request = conversation_.build(shape.system, shape.tools, shape.params);
                estimated = estimator_.estimate(request);
                sink(ContextUpdate{{}, estimated, context_limit});
                try {
                    reply = model_.complete(request, on_stream, on_retry, stop);
                    break;
                } catch (const ModelError& error) {
                    if (error.kind() != ModelError::Kind::context_too_long || attempt != 0) throw;
                    log_agent()->warn("服务端报上下文超长（估算 {} tokens），强制压缩后重发：{}", estimated,
                                      error.what());
                    compactor_.force(conversation_, shape, model_, estimator_, recorder_, sink, stop);
                    check_broken(sink);
                }
            }
        } catch (const ModelError& error) {
            switch (error.kind()) {
            case ModelError::Kind::cancelled:
                keep_partial(error.partial(), sink);
                return finish(TurnStatus::interrupted, "", steps, calls, total, sink);
            case ModelError::Kind::context_too_long:
                return finish(TurnStatus::failed, "上下文超出模型窗口，请用 /new 开始新会话", steps,
                              calls, total, sink);
            case ModelError::Kind::rejected:
            case ModelError::Kind::exhausted:
                return finish(TurnStatus::failed, error.what(), steps, calls, total, sink);
            }
        }

        if (reply.usage) {
            estimator_.observe_prompt_tokens(reply.usage->prompt);
            total.prompt += reply.usage->prompt;
            total.completion += reply.usage->completion;
            total.cached += reply.usage->cached;
        }
        const std::size_t used =
            reply.usage ? static_cast<std::size_t>(reply.usage->prompt + reply.usage->completion)
                        : estimated;
        sink(ContextUpdate{reply.usage.value_or(Usage{}), used, context_limit});

        if (reply.message.content.empty() && reply.message.tool_calls.empty()) {
            sink(Notice{Notice::Level::warn, "模型返回了空回复"});
            return finish(TurnStatus::done, "", steps, calls, total, sink);
        }

        const std::int64_t assistant_ordinal = conversation_.add_assistant(reply.message);
        recorder_.assistant(assistant_ordinal, reply);
        check_broken(sink);

        if (reply.message.tool_calls.empty()) {
            if (grace) return finish(TurnStatus::limit, "", steps, calls, total, sink);
            if (reply.finish.reason == Finish::Reason::length) {
                sink(Notice{Notice::Level::warn, "回复达到 max_tokens 上限被截断，可以输入“继续”"});
            } else if (reply.finish.reason == Finish::Reason::content_filter) {
                sink(Notice{Notice::Level::warn, "回复被服务端的内容过滤截断"});
            }
            return finish(TurnStatus::done, "", steps, calls, total, sink);
        }

        const DispatchOutcome outcome =
            dispatch(reply.message.tool_calls, max_tool_calls - calls, sink, approver, stop);
        calls += outcome.handled;
        switch (outcome.stop) {
        case DispatchOutcome::Stop::interrupted:
            return finish(TurnStatus::interrupted, "", steps, calls, total, sink);
        case DispatchOutcome::Stop::denied:
            return finish(TurnStatus::denied, "", steps, calls, total, sink);
        case DispatchOutcome::Stop::none:
            if (grace) return finish(TurnStatus::limit, "", steps, calls, total, sink);
            if (outcome.hit_limit) grace = true;
            break;
        }
    }
}

TurnStatus Agent::compact(const Sink& sink, std::stop_token stop) {
    TurnStatus status = TurnStatus::done;
    try {
        compactor_.summarize(conversation_, {system_prompt_, tool_defs(), setup_.model}, model_,
                             estimator_, recorder_, sink, stop);
        sink(ContextUpdate{{}, estimator_.estimate(conversation_.build(system_prompt_, tool_defs(),
                                                                       setup_.model)),
                            compactor_.budget().limit});
    } catch (const ModelError& error) {
        status = error.kind() == ModelError::Kind::cancelled ? TurnStatus::interrupted : TurnStatus::failed;
        if (status == TurnStatus::failed) sink(Notice{Notice::Level::error, error.what()});
    }
    recorder_.sync();
    check_broken(sink);
    return status;
}

void Agent::set_permission_mode(PermissionMode mode) { policy_.set_mode(mode); }

Agent::~Agent() { recorder_.sync(); }

} // namespace dagent::agent
