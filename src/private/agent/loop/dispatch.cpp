#include "agent/loop/dispatch.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <format>
#include <future>
#include <condition_variable>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "agent/policy/authorization.hpp"
#include "agent/loop/catalog.hpp"
#include "agent/record/committer.hpp"
#include "agent/policy/decision_review.hpp"
#include "agent/policy/permission.hpp"
#include "agent/policy/parent_review.hpp"
#include "agent/log.hpp"
#include "base/text.hpp"

namespace dagent::agent {
namespace {

// Worker 等待所属执行线程的答复；仅请求求值映射异常，promise 的交付仍在 catch 之外。
template<class Request, class OnError>
auto mailbox_roundtrip(ExecutionMailbox& mailbox, Request request, OnError on_error) {
    using Result = std::invoke_result_t<Request&>;
    auto promise = std::make_shared<std::promise<Result>>();
    auto reply = promise->get_future();
    mailbox.post([request = std::move(request), on_error = std::move(on_error), promise]() mutable {
        Result result;
        try { result = request(); }
        catch (const std::exception& error) { result = on_error(error); }
        promise->set_value(std::move(result));
    });
    return reply.get();
}

// prepare 失败的调用没有 Intent：用「工具名 + 参数前 60 字节（退到 UTF-8 字符边界）」当摘要。
std::string fallback_summary(const ToolCall& call) {
    std::string args = call.arguments.substr(0, base::utf8_floor(call.arguments, 60));
    std::ranges::replace(args, '\n', ' ');
    std::ranges::replace(args, '\r', ' ');
    if (!call.name.empty() && !args.empty()) return call.name + " " + args;
    return call.name.empty() ? args : call.name;
}

std::string summary_of(const PreparedAction& action) {
    if (const auto* control = std::get_if<ControlRequest>(&action)) {
        return std::visit([](const auto& request) { return request.summary; }, *control);
    }
    return std::get<std::unique_ptr<PreparedTool>>(action)->intent().summary;
}

// 一次 dispatch 的状态与回调；所有 worker 和邮箱处理结束后才销毁。
class DispatchBatch {
    using Outcome = ActionDispatcher::Outcome;

    struct Slot {
        const ToolCall* call = nullptr;
        std::string summary;
        std::optional<PreparedAction> action; ///< 已准备的动作（普通工具或控制请求）
        std::optional<ToolResult> result;     ///< 有值 = 可以提交
        std::shared_ptr<ActiveExecution> execution; ///< 活跃执行登记（普通工具）
    };
    struct Pending {
        std::size_t slot;
        ExecutionGrant grant;
    };
    enum class GroupKind { readonly, task };

    struct ResetReviewer {
        ResetReviewer(ControlActionExecutor& control, const Approver* parent_reviewer)
            : control(control) {
            control.set_parent_reviewer(parent_reviewer);
        }
        ControlActionExecutor& control;
        ~ResetReviewer() { control.set_parent_reviewer(nullptr); }
    };

    Session& session_;
    const RunServices& services_;
    const std::string& root_turn_id_; ///< 借用 ActionDispatcher 的整轮标识，不按批次生成。
    int budget;
    SessionCommitter& committer;
    Policy& policy;
    ExecutionRegistry& executions;
    ControlActionExecutor& control;
    ActionCatalog& catalog;
    SessionResources* resources;
    const Sink& sink;
    const std::stop_token stop;
    ExecutionMailbox mailbox;
    ParentReviewer reviewer;
    const Approver parent_reviewer;
    // 保持原局部变量的析构顺序：后续批次状态先销毁，再复位，最后销毁回调、reviewer 和 mailbox。
    ResetReviewer reset_reviewer;
    std::vector<Slot> slots;
    Outcome outcome;
    std::size_t committed = 0;
    std::vector<Pending> group; ///< 挂起的并行组
    GroupKind group_kind = GroupKind::readonly;
    // 决策审阅：只有配置了决策模型、且本轮自动审批未被关闭时才尝试代批（计划 2.2/2.9）。
    const std::shared_ptr<DecisionServices>& decisions;
    const bool decision_ready;
    std::optional<DecisionReviewer> decision_reviewer;
    const ReviewContext review_context;
    ApprovalSupport approvals;
    ActionAuthorizer action_authorizer;

public:
    DispatchBatch(Session& session, Run& run, const RunServices& services,
                  const std::string& root_turn_id, const std::vector<ToolCall>& calls, int budget)
        : session_(session), services_(services), root_turn_id_(root_turn_id), budget(budget),
          committer(session_.committer()), policy(session_.policy()), executions(session_.executions()),
          control(session_.control()), catalog(session_.catalog()), resources(services_.resources),
          sink(services_.sink), stop(services_.stop), reviewer(session_, run, services_),
          parent_reviewer([this](Approval& approval, std::stop_token child_stop) {
              return reviewer.submit(mailbox, approval, child_stop);
          }),
          reset_reviewer(control, session_.config().options.approval.mode != ParentReviewMode::disabled
                                      ? &parent_reviewer : nullptr),
          // 槽位与摘要仍在注册父审批之后、初始化决策审阅之前创建；执行期间不再移动槽位。
          slots([&calls] {
              std::vector<Slot> slots(calls.size());
              for (std::size_t i = 0; i < calls.size(); ++i) {
                  slots[i].call = &calls[i];
                  slots[i].summary = fallback_summary(calls[i]);
              }
              return slots;
          }()),
          decisions(session_.decision()), decision_ready(decisions != nullptr && decisions->enabled()),
          review_context(session_.review_context()),
          approvals(policy, committer,
                    {session_.meta().id, session_.config().cwd, session_.is_main(), session_.config().session_started},
                    decisions, decision_ready, decision_reviewer, services_.approver, services_.review_gate, stop),
          action_authorizer(approvals) {
        if (decision_ready) decision_reviewer.emplace(*decisions, review_context, root_turn_id_);
    }

private:
    // 并行资格：普通工具按既有 parallel()；控制动作按类型（todo 只读组、task 任务组、问答串行等待）。
    std::optional<GroupKind> parallel_kind(const std::expected<PreparedAction, ToolResult>& prepared,
                                          const std::optional<Verdict>& verdict) {
        if (!prepared) return std::nullopt;
        if (const auto* control_request = std::get_if<ControlRequest>(&prepared.value())) {
            if (std::holds_alternative<PlanReplacement>(*control_request)) return GroupKind::readonly;
            if (std::holds_alternative<DelegationRequest>(*control_request)) return GroupKind::task;
            return std::nullopt;
        }
        if (!verdict) return std::nullopt;
        const auto& tool = std::get<std::unique_ptr<PreparedTool>>(prepared.value());
        if (!parallel(*verdict, tool->intent())) return std::nullopt;
        return GroupKind::readonly;
    }

    // 结果按 tool_calls 的原始顺序写入历史、写记录、发事件；能提交的前缀尽早提交（docs/design/agent.md §6）。
    void commit() {
        while (committed < slots.size() && slots[committed].result.has_value()) {
            Slot& slot = slots[committed];
            for (const ExecutionSignal& signal : slot.result->signals) {
                if (const auto* disconnected = std::get_if<McpDisconnected>(&signal)) {
                    // 告诉模型这个 server 会重连还是已不可用，免得它在工具消失后反复寻找。
                    if (resources != nullptr) {
                        slot.result->model_text +=
                            resources->mark_disconnected(disconnected->server, slot.result->model_text);
                    }
                }
            }
            const TodoView* plan = nullptr;
            if (slot.action) {
                if (const auto* control_request = std::get_if<ControlRequest>(&slot.action.value())) {
                    if (const auto* replacement = std::get_if<PlanReplacement>(control_request)) {
                        plan = &replacement->plan; // 与 tool 记录同一次提交
                    }
                }
            }
            committer.commit_tool(*slot.call, slot.summary, *slot.result, plan);
            ++committed;
        }
    }

    auto make_on_output(const ToolCall& call) {
        return [this, id = call.id](std::string_view chunk) { sink(ToolOutput{id, std::string(chunk)}); };
    }

    // 权限变化终止执行后，用说明替换"被用户中断"，避免把系统降权误报成用户取消。
    void finish_execution(Slot& slot) {
        if (slot.execution == nullptr) return;
        if (slot.result) {
            if (auto* bash = std::get_if<BashView>(&slot.result->display)) {
                for (const auto& target : slot.execution->network_targets()) {
                    const auto key = target.host + ":" + std::to_string(target.port);
                    if (std::ranges::find(bash->network_targets, key) == bash->network_targets.end())
                        bash->network_targets.push_back(key);
                }
            }
        }
        if (slot.result) {
            if (auto* web = std::get_if<WebView>(&slot.result->display)) {
                for (const auto& target : slot.execution->network_targets())
                    web->network_targets.push_back(target.host + ":" + std::to_string(target.port));
            }
        }
        std::string reason = slot.execution->termination_reason();
        if (reason.empty() && slot.execution->grant.authority_stop.stop_requested())
            reason = "parent permission authority was revoked";
        if (!reason.empty() && slot.result) {
            auto& text = slot.result->model_text;
            constexpr std::string_view interruption = "\n[interrupted by the user]";
            if (slot.result->interrupted && text.ends_with(interruption))
                text.resize(text.size() - interruption.size());
            if (!text.empty()) text += '\n';
            text += std::format(texts::kPermissionTerminated, reason);
            slot.result->is_error = true;
            slot.result->interrupted = false;
            if (auto* bash = std::get_if<BashView>(&slot.result->display)) bash->interrupted = false;
            if (auto* web = std::get_if<WebView>(&slot.result->display)) web->output = text;
        }
        executions.end(slot.execution);
        slot.execution.reset();
    }

    std::expected<ExecutionGrant, ToolResult> authorize(const ToolCall& call, const PreparedIntent& intent) {
        auto result = action_authorizer.authorize(call, intent);
        if (!result) {
            switch (result.error().stop) {
            case ActionAuthorizer::Failure::Stop::none: break;
            case ActionAuthorizer::Failure::Stop::interrupted: outcome.stop = Outcome::Stop::interrupted; break;
            case ActionAuthorizer::Failure::Stop::denied: outcome.stop = Outcome::Stop::denied; break;
            }
            return std::unexpected(std::move(result.error().result));
        }
        return std::move(*result);
    }

    ToolResult execute(Slot& slot, const ExecutionGrant& grant) {
        if (const auto* control_request = std::get_if<ControlRequest>(&slot.action.value()))
            return control.execute(*control_request, stop);
        ExecutionGrant scoped = grant;
        if (scoped.revision != policy.revision() || scoped.authority_stop.stop_requested())
            return make_result("Permission changed before execution; prepare the operation again.", true, false);
        // Parallel read-only groups cannot request additional permissions.
        scoped.network_decider = {};
        const std::stop_token execution_stop =
            slot.execution != nullptr ? slot.execution->stop.get_token() : stop;
        return std::get<std::unique_ptr<PreparedTool>>(slot.action.value())
            ->execute(scoped, make_on_output(*slot.call), execution_stop);
    }

    // 串行执行：普通工具写 tool_started 审计；ask/exit_plan 只发实时开始、不写记录。
    void run_serial(std::size_t i, ExecutionGrant grant) {
        Slot& slot = slots[i];
        grant.execution_id = approvals.execution_id(slot.call->id);
        const bool ordinary = std::holds_alternative<std::unique_ptr<PreparedTool>>(slot.action.value());
        const ToolStarted started{slot.call->id, slot.call->name, slot.summary, grant};
        if (ordinary && (grant.revision != policy.revision() || grant.authority_stop.stop_requested())) {
            slot.result = make_result("Permission changed before execution; prepare the operation again.", true, false);
            return;
        }
        if (ordinary) {
            const auto& tool = *std::get<std::unique_ptr<PreparedTool>>(*slot.action);
            slot.execution = executions.begin(*slot.call, tool.intent(), grant, stop);
            if (grant.revision != policy.revision() || slot.execution->stop.stop_requested()) {
                slot.result = make_result("Permission changed before execution; prepare again.", true, false);
                finish_execution(slot);
                return;
            }
            committer.commit_tool_started(started);
        }
        sink(started);
        if (!ordinary) {
            slot.result = execute(slot, grant);
        } else {
            // 地址在本次执行中固定；此作用域结束时先 join worker，再销毁授权协作者。
            ExecutionAuthorizer authorization(approvals, *slot.call,
                *std::get<std::unique_ptr<PreparedTool>>(*slot.action), slot.execution);
            ExecutionGrant scoped = grant;
            scoped.network_decider = [&](const NetworkTarget& target, std::string& reason, std::stop_token execution_stop) {
                auto [action, detail] = mailbox_roundtrip(mailbox,
                    [&, target, execution_stop] {
                        std::string detail;
                        NetworkAction action = authorization.decide_network(target, detail, execution_stop);
                        return std::pair{action, std::move(detail)};
                    },
                    [](const std::exception& error) {
                        return std::pair{NetworkAction::cancel, std::string(error.what())};
                    });
                reason = std::move(detail);
                return action;
            };
            if (approvals.decision_allowed()) {
                scoped.web_request_decider = [&](const WebRequestIntent& request, std::stop_token execution_stop) {
                    return mailbox_roundtrip(mailbox,
                        [&, request, execution_stop] {
                            return authorization.decide_web_request(request, execution_stop);
                        },
                        [](const std::exception& error) {
                            WebRequestAccess access;
                            access.action = NetworkAction::cancel;
                            access.reason = error.what();
                            return access;
                        });
                };
                scoped.web_result_scanner = [&](const WebScanInput& input, std::stop_token execution_stop) {
                    return mailbox_roundtrip(mailbox,
                        [&, input, execution_stop] {
                            return authorization.scan_web_result(input, execution_stop);
                        },
                        [](const std::exception& error) {
                            ScanResult result;
                            result.kind = ScanResult::Kind::unavailable;
                            result.message = error.what();
                            return result;
                        });
                };
            }
            const std::stop_token execution_stop = slot.execution->stop.get_token();
            mailbox.start(1);
            std::jthread worker([&] {
                try {
                    slot.result = std::get<std::unique_ptr<PreparedTool>>(*slot.action)
                        ->execute(scoped, make_on_output(*slot.call), execution_stop);
                } catch (const std::exception& error) {
                    slot.result = make_result(error.what(), true, false);
                }
                mailbox.finished();
            });
            mailbox.drain();
        }
        finish_execution(slot);
        if (!ordinary && slot.result->interrupted) outcome.stop = Outcome::Stop::interrupted;
    }

    // 并行组：ToolStarted 按顺序在 agent 线程上发；执行分块、每块至多 width 个 jthread，
    // 块内 join 完才起下一块；每个线程只写自己的 slot（docs/design/agent.md §6）。
    void run_group() {
        if (group.empty()) return;
        const std::size_t width = group_kind == GroupKind::task
                                      ? static_cast<std::size_t>(
                                            session_.config().options.run.max_parallel_tasks)
                                      : static_cast<std::size_t>(session_.config().options.run.max_parallel_tools);
        for (Pending& p : group) {
            Slot& slot = slots[p.slot];
            p.grant.execution_id = approvals.execution_id(slot.call->id);
            const ToolStarted started{slot.call->id, slot.call->name, slot.summary, p.grant};
            if (std::holds_alternative<std::unique_ptr<PreparedTool>>(slot.action.value())) {
                const auto& tool = *std::get<std::unique_ptr<PreparedTool>>(slot.action.value());
                slot.execution = executions.begin(*slot.call, tool.intent(), p.grant, stop);
            }
            committer.commit_tool_started(started);
            sink(started);
        }
        for (std::size_t base = 0; base < group.size(); base += width) {
            const std::size_t end = std::min(base + width, group.size());
            std::vector<std::jthread> threads;
            threads.reserve(end - base);
            mailbox.start(end - base);
            for (std::size_t k = base; k < end; ++k) {
                Pending& p = group[k];
                Slot& slot = slots[p.slot];
                threads.emplace_back([&, grant = p.grant] {
                    try { slot.result = execute(slot, grant); }
                    catch (const std::exception& error) { slot.result = make_result(error.what(), true, false); }
                    mailbox.finished();
                });
            }
            mailbox.drain();
        } // jthread 析构时 join：这一块全部结束才开始下一块
        for (const Pending& p : group) finish_execution(slots[p.slot]);
        group.clear();
    }

public:
    Outcome run() {
        for (std::size_t i = 0; i < slots.size(); ++i) {
            Slot& slot = slots[i];
            if (outcome.stop == Outcome::Stop::interrupted || stop.stop_requested()) {
                outcome.stop = Outcome::Stop::interrupted;
                slot.result = make_result(std::string(texts::kInterruptedCall), false, true);
                continue;
            }
            if (outcome.stop == Outcome::Stop::denied) {
                slot.result = make_result(std::string(texts::kPriorDenied), true, false);
                continue;
            }
            if (outcome.handled == budget) {
                slot.result = make_result(
                    std::format(texts::kToolLimit, session_.config().options.run.max_tool_calls), true, false);
                outcome.hit_limit = true;
                continue;
            }
            ++outcome.handled;

            auto prepared = catalog.prepare(slot.call->name, slot.call->arguments, slot.call->id);
            std::optional<Verdict> verdict;
            if (prepared) {
                if (const auto* tool = std::get_if<std::unique_ptr<PreparedTool>>(&prepared.value()))
                    verdict = policy.evaluate(*slot.call, (*tool)->intent());
            }
            if (!prepared && !catalog.contains(slot.call->name)) {
                slot.result = std::move(prepared.error());
                commit();
                continue;
            }
            // 这个调用进不了并行组（或类别不同）：先跑完挂起的组，再重新 prepare（prepare 无副作用，docs/design/agent.md §6）。
            std::optional<GroupKind> kind = parallel_kind(prepared, verdict);
            if (!group.empty() && (!kind || *kind != group_kind)) {
                run_group();
                prepared = catalog.prepare(slot.call->name, slot.call->arguments, slot.call->id);
                verdict.reset();
                if (prepared) {
                    if (const auto* tool = std::get_if<std::unique_ptr<PreparedTool>>(&prepared.value()))
                        verdict = policy.evaluate(*slot.call, (*tool)->intent());
                }
                kind = parallel_kind(prepared, verdict);
            }
            if (!prepared) {
                slot.result = std::move(prepared.error());
                commit();
                continue;
            }
            slot.action = std::move(prepared.value());
            slot.summary = summary_of(*slot.action); // 以最后一次 prepare 的 Intent 为准

            if (std::holds_alternative<ControlRequest>(*slot.action)) {
                if (kind) {
                    group.push_back(Pending{i, {}});
                    group_kind = *kind;
                } else {
                    run_serial(i, {});
                }
                commit();
                continue;
            }

            auto& tool = *std::get<std::unique_ptr<PreparedTool>>(*slot.action);
            if (verdict->kind != Verdict::Kind::deny) {
                if (const auto request = tool.preview_request()) {
                    auto read_grant = authorize(*slot.call, *request);
                    if (!read_grant) slot.result = std::move(read_grant.error());
                    else if (read_grant->revision != policy.revision() || stop.stop_requested())
                        slot.result = make_result("Permission changed before preview; prepare again.", true, false);
                    else slot.result = tool.prepare_preview(*read_grant);
                    if (slot.result) { commit(); continue; }
                    verdict = policy.evaluate(*slot.call, tool.intent());
                    slot.summary = tool.intent().summary;
                }
            }

            const Verdict& v = *verdict;
            log_agent()->debug("工具调用 {}：{} → {}", slot.call->name, slot.summary,
                               v.kind == Verdict::Kind::allow    ? "allow"
                               : v.kind == Verdict::Kind::ask    ? "ask"
                                                                 : "deny");

            switch (v.kind) {
            case Verdict::Kind::allow:
                if (kind) {
                    group.push_back(Pending{i, v.grant});
                    group_kind = *kind;
                } else {
                    run_serial(i, v.grant);
                }
                break;
            case Verdict::Kind::deny:
                slot.result = make_result(std::format(texts::kPolicyDenied, v.reason), true, false);
                break;
            case Verdict::Kind::ask: {
                auto grant = authorize(*slot.call, tool.intent());
                if (grant) run_serial(i, *grant);
                else slot.result = std::move(grant.error());
                break;
            }
            }
            commit();
        }
        run_group();
        commit();
        // 最后一个调用执行中被中断时，循环里没有下一个调用来发现 stop。
        if (outcome.stop == Outcome::Stop::none && stop.stop_requested()) {
            outcome.stop = Outcome::Stop::interrupted;
        }
        assert(committed == slots.size());
        return outcome;
    }
};

} // namespace

ActionDispatcher::Outcome ActionDispatcher::dispatch(const std::vector<ToolCall>& calls, int budget) {
    DispatchBatch batch(session_, run_, services_, root_turn_id_, calls, budget);
    return batch.run();
}

} // namespace dagent::agent
