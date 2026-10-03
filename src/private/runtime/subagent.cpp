#include "runtime/subagent.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
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
    ChildExecution(SubagentExecutor& owner, SessionFactory& factory, const agent::DelegationContext& context,
                   const agent::SubagentDef& def, const agent::DerivedPermission& permission,
                   const agent::DelegationRequest& request, std::stop_token parent_authority)
        : owner_(owner), factory_(factory), context_(context), def_(def), permission_(permission),
          prompt_(request.prompt), parent_authority_(parent_authority) {}

    agent::ToolResult run() {
        instance_ = factory_.create_child(context_, def_, permission_);
        agent::Session& session = instance_->session();
        view_.agent = def_.name;
        view_.task = prompt_;
        view_.session_id = session.meta().id;

        // 子会话独立取消来源：权限核对终止子执行时不连带父 turn；父 stop 仍然级联。
        const std::shared_ptr<std::stop_source> child_stop =
            owner_.attach(session.policy(), context_);
        struct Detach {
            SubagentExecutor& owner;
            agent::Policy& policy;
            ~Detach() { owner.detach(policy); }
        } detach{owner_, session.policy()};
        const std::stop_callback relay(context_.stop, [child_stop] { child_stop->request_stop(); });
        // 父 unrestricted 降为 workspace 时，子的生效模式仍可能是 workspace。
        // 依赖父授权的运行必须取消；创建期间失效的 token 注册后也会立即触发。
        const std::stop_callback authority_relay(parent_authority_,
                                                 [child_stop] { child_stop->request_stop(); });

        const agent::Sink child_sink = [this](const agent::Event& event) { on_child_event(event); };
        agent::Approver approver;
        if (permission_.may_ask) {
            approver = [this, child_stop](agent::Approval& approval, std::stop_token stop) {
                approval.agent = def_.name;
                approval.origin_call_id = context_.call_id;
                approval.delegated_task = prompt_;
                approval.identity.parent_session_id = context_.parent_session_id;
                approval.identity.child_session_id = view_.session_id;
                approval.identity.origin_call_id = context_.call_id;

                if (stop.stop_requested() || context_.stop.stop_requested() ||
                    parent_authority_.stop_requested()) {
                    agent::Decision decision;
                    decision.answer = agent::Decision::Answer::deny_with_feedback;
                    decision.feedback = "Subagent permission request cancelled: parent stopped or authority revoked.";
                    decision.state = "cancelled";
                    return decision;
                }

                if (context_.parent_policy != nullptr) {
                    const auto parent = context_.parent_policy->authority_view();
                    approval.identity.parent_revision = parent.revision;
                    if (parent.can_review() && context_.parent_reviewer != nullptr &&
                        *context_.parent_reviewer) {
                        approval.authority = agent::ApprovalAuthority::parent_model;
                        approval.authority_stop = parent.stop;
                        // 创建时父可能尚未 unrestricted；首次代审后也须保留撤权依赖，
                        // 覆盖运行中网络审批及随后命中的子会话授权。
                        parent_review_stop_ =
                            std::make_unique<std::stop_callback<std::function<void()>>>(
                                parent.stop, [child_stop] { child_stop->request_stop(); });
                        // 邮箱在父 Dispatcher 等待 task 时处理。拒绝、超时或撤权直接反馈，
                        // 同一个请求绝不能降级为人工弹窗重新申请。
                        return (*context_.parent_reviewer)(approval, stop);
                    }
                }

                approval.authority = agent::ApprovalAuthority::user;
                if (context_.approver != nullptr && *context_.approver)
                    return (*context_.approver)(approval, stop);

                agent::Decision decision;
                decision.answer = agent::Decision::Answer::deny_with_feedback;
                decision.feedback = "Permission approval unavailable: no user approval channel and no eligible parent reviewer.";
                decision.state = "denied";
                return decision;
            };
        }
        const agent::Asker asker{};

        agent::RunServices services{child_sink, approver, asker, nullptr, &instance_->resources(),
                                    child_stop->get_token()};
        agent::Run run;

        const auto began = std::chrono::steady_clock::now();
        session.begin_run(services, run);
        agent::TurnRunner runner;
        const agent::RunOutcome outcome = runner.run(session, run, services, prompt_);
        session.end_run();
        seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

        view_.interrupted = view_.interrupted || child_stop->stop_requested();
        view_.seconds = seconds_;

        agent::ToolResult result;
        result.interrupted = view_.interrupted;
        if (outcome.status != agent::TurnStatus::done) {
            result.model_text = std::format("Subagent {} ended with status={} (model_calls={}, tool_calls={}).",
                                            def_.name, agent::to_string(outcome.status),
                                            outcome.steps, outcome.tool_calls);
            if (!outcome.error.empty()) result.model_text += "\n" + outcome.error;
            if (!view_.result.empty()) result.model_text += "\nPartial output:\n" + view_.result;
            result.is_error = true;
        } else if (view_.result.empty()) {
            result.model_text = std::format("Subagent {} produced no conclusion", def_.name);
            result.is_error = true;
        } else {
            result.model_text = view_.result;
        }
        view_.result = result.model_text;
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

    SubagentExecutor& owner_;
    SessionFactory& factory_;
    const agent::DelegationContext& context_;
    const agent::SubagentDef& def_;
    const agent::DerivedPermission& permission_;
    std::string prompt_;
    std::stop_token parent_authority_;
    std::unique_ptr<SessionInstance> instance_;
    std::unique_ptr<std::stop_callback<std::function<void()>>> parent_review_stop_;
    std::mutex mutex_;
    std::string current_text_;
    agent::TaskView view_;
    double seconds_ = 0;
};

} // namespace

agent::ToolResult SubagentExecutor::delegate(const agent::DelegationContext& context,
                                             const agent::DelegationRequest& request) {
    const agent::SubagentDef* def = factory_.find_subagent(request.agent);
    if (def == nullptr) return error_result(std::format("unknown subagent: {}", request.agent));

    agent::DelegationContext live = context;
    std::stop_token parent_authority;
    if (context.parent_policy != nullptr) {
        const auto parent = context.parent_policy->authority_view();
        live.parent_mode = parent.permission.mode;
        live.parent_read_only = parent.permission.read_only;
        live.parent_planning = parent.permission.planning;
        if (parent.permission.mode == agent::PermissionMode::unrestricted)
            parent_authority = parent.stop;
    }
    const agent::DerivedPermission permission = agent::derive_permission(
        live.parent_mode, live.parent_planning, live.parent_read_only, def->permission);

    try {
        ChildExecution child(*this, factory_, live, *def, permission, request, parent_authority);
        return child.run();
    } catch (const std::exception& error) {
        base::logger("runtime")->warn("子 Agent {} 启动失败：{}", def->name, error.what());
        return error_result(std::format("failed to start subagent {}: {}", def->name, error.what()));
    }
}

std::shared_ptr<std::stop_source> SubagentExecutor::attach(agent::Policy& policy,
                                                          const agent::DelegationContext& context) {
    auto stop = std::make_shared<std::stop_source>();
    const std::lock_guard lock(mutex_);
    // 与 apply_parent_permission 共用登记锁：创建子实例期间发生的父降权不能漏过。
    const auto parent = context.parent_policy != nullptr
                            ? context.parent_policy->authority_view()
                            : agent::Policy::AuthorityView{
                                  {context.parent_mode, context.parent_read_only, context.parent_planning}, 0, {}};
    const auto effective = policy.set_parent_cap(parent.permission.mode, parent.permission.read_only,
                                                 parent.permission.planning);
    children_.push_back({&policy, stop, effective, context.parent_policy, parent.revision,
                         parent.can_review()});
    return stop;
}

void SubagentExecutor::detach(agent::Policy& policy) {
    const std::lock_guard lock(mutex_);
    std::erase_if(children_, [&](const ActiveChild& child) { return child.policy == &policy; });
}

void SubagentExecutor::apply_parent_permission(agent::PermissionMode mode, bool read_only, bool planning) {
    std::vector<std::shared_ptr<std::stop_source>> stopped;
    {
        const std::lock_guard lock(mutex_);
        // detach uses this lock too: keep each policy alive until its cap is updated.
        for (ActiveChild& child : children_) {
            const auto parent = child.parent_policy != nullptr
                                    ? child.parent_policy->authority_view()
                                    : agent::Policy::AuthorityView{{mode, read_only, planning}, 0, {}};
            const auto after = child.policy->set_parent_cap(parent.permission.mode,
                                                            parent.permission.read_only,
                                                            parent.permission.planning);
            const bool revoked = child.parent_review && parent.revision != child.parent_revision;
            if (revoked || agent::narrower(after, child.effective)) stopped.push_back(child.stop);
            child.effective = after;
        }
    }
    for (const auto& stop : stopped) {
        base::logger("runtime")->info("父权限收窄或审批授权失效；终止依赖旧权限的子执行");
        stop->request_stop();
    }
}

} // namespace dagent::runtime
