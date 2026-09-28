#include "runtime/subagent.hpp"

#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <utility>
#include <variant>

#include "agent/run.hpp"
#include "agent/turn_runner.hpp"
#include "base/log.hpp"

namespace dagent::runtime {
namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

agent::ToolResult error_result(std::string text) {
    agent::ToolResult result;
    result.model_text = std::move(text);
    result.is_error = true;
    return result;
}

/// @brief 一次委派的子执行对象：拥有子实例与子 Run，随本次调用结束销毁（不留下可再运行句柄）。
class ChildExecution {
public:
    ChildExecution(SessionFactory& factory, const agent::DelegationContext& context,
                   const agent::SubagentDef& def, const agent::DerivedPermission& permission,
                   const agent::DelegationRequest& request)
        : factory_(factory), context_(context), def_(def), permission_(permission),
          prompt_(request.prompt) {}

    agent::ToolResult run() {
        instance_ = factory_.create_child(context_, def_, permission_);
        agent::Session& session = instance_->session();
        view_.agent = def_.name;
        view_.task = prompt_;
        view_.session_id = session.meta().id;

        const agent::Sink child_sink = [this](const agent::Event& event) { on_child_event(event); };
        agent::Approver approver;
        if (permission_.may_ask && context_.approver != nullptr && *context_.approver) {
            approver = [this](const agent::Approval& approval, std::stop_token stop) {
                agent::Approval copy = approval;
                copy.agent = def_.name;
                copy.origin_call_id = context_.call_id;
                return (*context_.approver)(copy, stop);
            };
        }
        const agent::Asker asker{};

        agent::RunServices services{child_sink, approver, asker, nullptr, &instance_->resources(),
                                    context_.stop};
        agent::Run run;

        const auto began = std::chrono::steady_clock::now();
        session.begin_run(services, run);
        agent::TurnRunner runner;
        runner.run(session, run, services, prompt_);
        session.end_run();
        seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

        {
            const std::lock_guard lock(mutex_);
            if (view_.result.empty()) view_.result = failure_;
        }
        view_.interrupted = view_.interrupted || context_.stop.stop_requested();
        view_.seconds = seconds_;

        agent::ToolResult result;
        result.interrupted = view_.interrupted;
        if (view_.result.empty()) {
            result.model_text = std::format("Subagent {} produced no conclusion", def_.name);
            if (!failure_.empty()) result.model_text += ": " + failure_;
            result.is_error = true;
        } else {
            result.model_text = view_.result;
            result.is_error = !failure_.empty();
        }
        result.display = std::move(view_);
        return result;
    }

private:
    void on_child_event(const agent::Event& event) {
        {
            const std::lock_guard lock(mutex_);
            std::visit(Overloaded{
                           [&](const agent::StepStarted&) { current_text_.clear(); },
                           [&](const agent::TextDelta& delta) { current_text_ += delta.text; },
                           [&](const agent::StreamReset&) { current_text_.clear(); },
                           [&](const agent::ToolFinished& finished) {
                               view_.steps.push_back({finished.summary, finished.result.is_error});
                           },
                           [&](const agent::TurnEnded& ended) {
                               view_.model_calls = ended.steps;
                               view_.tool_calls = ended.tool_calls;
                               if (ended.status == agent::TurnStatus::interrupted) view_.interrupted = true;
                               if (!current_text_.empty()) view_.result = current_text_;
                               if (ended.status == agent::TurnStatus::failed && !ended.error.empty())
                                   failure_ = ended.error;
                           },
                           [&](const auto&) {},
                       },
                       event);
        }
        if (context_.sink != nullptr && *context_.sink) {
            (*context_.sink)(agent::SubEvent{view_.session_id, def_.name, context_.call_id,
                                             std::make_shared<const agent::EventBox>(
                                                 agent::EventBox{event})});
        }
    }

    SessionFactory& factory_;
    const agent::DelegationContext& context_;
    const agent::SubagentDef& def_;
    const agent::DerivedPermission& permission_;
    std::string prompt_;
    std::unique_ptr<SessionInstance> instance_;
    std::mutex mutex_;
    std::string current_text_, failure_;
    agent::TaskView view_;
    double seconds_ = 0;
};

} // namespace

agent::ToolResult SubagentExecutor::delegate(const agent::DelegationContext& context,
                                             const agent::DelegationRequest& request) {
    const agent::SubagentDef* def = factory_.find_subagent(request.agent);
    if (def == nullptr) return error_result(std::format("unknown subagent: {}", request.agent));

    const agent::DerivedPermission permission = agent::derive_permission(
        context.parent_mode, context.parent_planning, context.parent_read_only, def->permission);

    try {
        ChildExecution child(factory_, context, *def, permission, request);
        return child.run();
    } catch (const std::exception& error) {
        base::logger("runtime")->warn("子 Agent {} 启动失败：{}", def->name, error.what());
        return error_result(std::format("failed to start subagent {}: {}", def->name, error.what()));
    }
}

} // namespace dagent::runtime
