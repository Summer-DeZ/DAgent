#include "agent/turn_runner.hpp"

#include <format>
#include <utility>

#include "agent/compaction.hpp"
#include "agent/committer.hpp"
#include "agent/dispatch.hpp"
#include "base/log.hpp"
#include "base/text.hpp"

namespace dagent::agent {
namespace {

std::shared_ptr<spdlog::logger> log_agent() { return base::logger("agent"); }

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

void report_stream(const StreamEvent& event, const Sink& sink) {
    std::visit(Overloaded{
                   [&](const TextDelta& delta) { sink(TextDelta{delta.text}); },
                   [&](const ReasoningDelta& delta) { sink(ReasoningDelta{delta.text}); },
                   [&](const ToolCallBegin& begin) { sink(ToolPending{begin.id, begin.name}); },
                   [&](const auto&) {},
               },
               event);
}

void report_retry(const RetryInfo& info, const Sink& sink) {
    sink(Retrying{info.attempt, info.max_attempts, info.wait, info.reason});
    if (info.had_output) sink(StreamReset{});
}

} // namespace

RunOutcome TurnRunner::finish(Session& session, Run& run, const RunServices& services, TurnStatus status,
                              std::string error) {
    session.committer().repair_open_calls(); // 补未闭合调用，不额外发 ToolFinished
    session.committer().record_turn_end(status, error, run.steps(), run.tool_calls(), run.usage());
    session.committer().sync();
    log_agent()->info("本轮结束：status={} steps={} tool_calls={} prompt={} completion={}",
                      to_string(status), run.steps(), run.tool_calls(), run.usage().prompt,
                      run.usage().completion);
    if (session.is_main() && services.resources != nullptr) services.resources->report_pending(services.sink);
    const RunOutcome outcome = run.finish(status, std::move(error));
    services.sink(TurnEnded{outcome.status, outcome.error, outcome.steps, outcome.tool_calls, outcome.usage});
    return outcome;
}

RunOutcome TurnRunner::run(Session& session, Run& run, const RunServices& base_services, std::string input) {
    RunServices services = base_services;
    services.stop = run.stop_token();
    const Sink& sink = services.sink;
    const std::stop_token stop = services.stop;

    input = base::to_valid_utf8(input);
    const auto mentions = skill_mentions(input);
    std::vector<SkillView> requested;
    for (const auto& name : mentions) {
        const auto* skill = session.config().skills ? session.config().skills->find(name) : nullptr;
        requested.push_back({name, skill ? skill->file.string() : std::string{}});
    }
    session.committer().commit_user(input, requested);
    sink(TurnStarted{input});
    for (const auto& name : mentions) {
        if (stop.stop_requested()) return finish(session, run, services, TurnStatus::interrupted, "");
        const auto result = session.activate_skill(name);
        if (result.is_error) return finish(session, run, services, TurnStatus::failed, result.model_text);
        sink(Notice{Notice::Level::info, "Loaded skill: " + name});
    }

    const int max_model_calls = session.config().options.run.max_model_calls;
    const int max_tool_calls = session.config().options.run.max_tool_calls;
    const std::size_t context_limit = session.compactor().budget().limit;

    ActionDispatcher dispatcher(session, run, services);

    const auto on_stream = [&](const StreamEvent& event) { report_stream(event, sink); };
    const auto on_retry = [&](const RetryInfo& info) { report_retry(info, sink); };

    for (;;) {
        if (run.steps() >= max_model_calls) {
            return finish(session, run, services, TurnStatus::limit,
                          "model call limit reached for this turn");
        }
        run.count_step();

        std::size_t estimated = 0;
        Reply reply;
        try {
            // MCP 的重连、等待、断线通报只由主会话做；子会话只用构造时的快照。
            if (session.is_main() && services.resources != nullptr)
                services.resources->begin_step(sink, stop);
            // 自动压缩在 StepStarted 之前（docs/design/agent.md §2）：界面在一步开始后作废的内容不含压缩提示。
            if (auto change = session.compactor().maybe_compact(session.conversation(), session.request_shape(),
                                                                session.model(), session.estimator(), sink,
                                                                stop)) {
                session.committer().commit_compaction(std::move(*change));
            }
            session.committer().check_broken();
            sink(StepStarted{run.steps()});
            for (int attempt = 0; ; ++attempt) {
                const Request request = session.build_request();
                estimated = session.estimator().estimate(request);
                sink(ContextUpdate{{}, estimated, context_limit});
                try {
                    reply = session.model().complete(request, on_stream, on_retry, stop);
                    break;
                } catch (const ModelError& error) {
                    if (error.kind() != ModelError::Kind::context_too_long || attempt != 0) throw;
                    log_agent()->warn("服务端报上下文超长（估算 {} tokens），强制压缩后重发：{}", estimated,
                                      error.what());
                    if (auto change = session.compactor().force(session.conversation(),
                                                                session.request_shape(), session.model(),
                                                                session.estimator(), sink, stop)) {
                        session.committer().commit_compaction(std::move(*change));
                    }
                    session.committer().check_broken();
                }
            }
        } catch (const ModelError& error) {
            switch (error.kind()) {
            case ModelError::Kind::cancelled:
                session.committer().commit_partial(error.partial().message.content);
                return finish(session, run, services, TurnStatus::interrupted, "");
            case ModelError::Kind::context_too_long:
                return finish(session, run, services, TurnStatus::failed,
                              "Context exceeds the model window - start a new session with /new");
            case ModelError::Kind::rejected:
            case ModelError::Kind::exhausted:
                return finish(session, run, services, TurnStatus::failed, error.what());
            }
        } catch (const ResourceError& error) {
            const bool cancelled = error.kind() == ResourceError::Kind::cancelled;
            return finish(session, run, services, cancelled ? TurnStatus::interrupted : TurnStatus::failed,
                          cancelled ? std::string{} : std::string(error.what()));
        }

        if (reply.usage) {
            session.estimator().observe_prompt_tokens(reply.usage->prompt);
            run.add_usage(*reply.usage);
        }
        const std::size_t used =
            reply.usage ? static_cast<std::size_t>(reply.usage->prompt + reply.usage->completion)
                        : estimated;
        sink(ContextUpdate{reply.usage.value_or(Usage{}), used, context_limit});

        if (reply.message.content.empty() && reply.message.tool_calls.empty()) {
            sink(Notice{Notice::Level::warn, "The model returned an empty reply"});
            return finish(session, run, services, TurnStatus::done, "");
        }

        session.committer().commit_assistant(reply);

        if (reply.message.tool_calls.empty()) {
            if (run.grace()) return finish(session, run, services, TurnStatus::limit, "");
            if (reply.finish.reason == Finish::Reason::length) {
                sink(Notice{Notice::Level::warn,
                            "Reply hit max_tokens and was cut off - send \"continue\" to resume"});
            } else if (reply.finish.reason == Finish::Reason::content_filter) {
                sink(Notice{Notice::Level::warn, "Reply was cut off by the provider's content filter"});
            }
            return finish(session, run, services, TurnStatus::done, "");
        }

        const ActionDispatcher::Outcome outcome =
            dispatcher.dispatch(reply.message.tool_calls, max_tool_calls - run.tool_calls());
        run.count_calls(outcome.handled);
        switch (outcome.stop) {
        case ActionDispatcher::Outcome::Stop::interrupted:
            return finish(session, run, services, TurnStatus::interrupted, "");
        case ActionDispatcher::Outcome::Stop::denied:
            return finish(session, run, services, TurnStatus::denied, "");
        case ActionDispatcher::Outcome::Stop::none:
            if (run.grace()) return finish(session, run, services, TurnStatus::limit, "");
            if (outcome.hit_limit) run.set_grace(true);
            break;
        }
    }
}

RunOutcome TurnRunner::compact(Session& session, Run& run, const RunServices& base_services) {
    RunServices services = base_services;
    services.stop = run.stop_token();
    const Sink& sink = services.sink;
    const std::stop_token stop = services.stop;

    TurnStatus status = TurnStatus::done;
    try {
        if (auto change = session.compactor().summarize(session.conversation(), session.request_shape(),
                                                        session.model(), session.estimator(), sink, stop)) {
            session.committer().commit_compaction(std::move(*change));
        }
        sink(ContextUpdate{{}, session.estimated_tokens(), session.compactor().budget().limit});
    } catch (const ModelError& error) {
        status = error.kind() == ModelError::Kind::cancelled ? TurnStatus::interrupted : TurnStatus::failed;
        if (status == TurnStatus::failed) sink(Notice{Notice::Level::error, error.what()});
    }
    session.committer().sync();
    session.committer().check_broken();
    return run.finish(status, "");
}

} // namespace dagent::agent
