#include "agent/dispatch.hpp"

#include <algorithm>
#include <cassert>
#include <format>
#include <map>
#include <future>
#include <condition_variable>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "agent/catalog.hpp"
#include "agent/committer.hpp"
#include "agent/permission.hpp"
#include "base/log.hpp"

namespace dagent::agent {
namespace {


std::shared_ptr<spdlog::logger> log_agent() { return base::logger("agent"); }

ToolResult make_result(std::string text, bool is_error, bool interrupted) {
    ToolResult result;
    result.model_text = std::move(text);
    result.is_error = is_error;
    result.interrupted = interrupted;
    return result;
}

// prepare 失败的调用没有 Intent：用「工具名 + 参数前 60 个字符」当摘要。
std::string fallback_summary(const ToolCall& call) {
    std::string args;
    for (const char c : call.arguments) {
        if (args.size() >= 60) break;
        args += (c == '\n' || c == '\r') ? ' ' : c;
    }
    if (!call.name.empty() && !args.empty()) return call.name + " " + args;
    return call.name.empty() ? args : call.name;
}

std::string summary_of(const PreparedAction& action) {
    if (const auto* control = std::get_if<ControlRequest>(&action)) {
        return std::visit([](const auto& request) { return request.summary; }, *control);
    }
    return std::get<std::unique_ptr<PreparedTool>>(action)->intent().summary;
}

} // namespace

ActionDispatcher::Outcome ActionDispatcher::dispatch(const std::vector<ToolCall>& calls, int budget) {
    SessionCommitter& committer = session_.committer();
    Policy& policy = session_.policy();
    ControlActionExecutor& control = session_.control();
    ActionCatalog& catalog = session_.catalog();
    SessionResources* resources = services_.resources;
    const Sink& sink = services_.sink;
    const std::stop_token stop = services_.stop;
    struct Slot {
        const ToolCall* call = nullptr;
        std::string summary;
        std::optional<PreparedAction> action; ///< 已准备的动作（普通工具或控制请求）
        std::optional<ToolResult> result;     ///< 有值 = 可以提交
    };
    struct Pending {
        std::size_t slot;
        ExecutionGrant grant;
    };

    std::vector<Slot> slots(calls.size());
    for (std::size_t i = 0; i < calls.size(); ++i) {
        slots[i].call = &calls[i];
        slots[i].summary = fallback_summary(calls[i]);
    }

    Outcome outcome;
    std::size_t committed = 0;
    enum class GroupKind { readonly, task };
    std::vector<Pending> group; ///< 挂起的并行组
    GroupKind group_kind = GroupKind::readonly;

    // 并行资格：普通工具按既有 parallel()；控制动作按类型（todo 只读组、task 任务组、问答串行等待）。
    const auto parallel_kind = [](const std::expected<PreparedAction, ToolResult>& prepared,
                                  const std::optional<Verdict>& verdict) -> std::optional<GroupKind> {
        if (!prepared) return std::nullopt;
        if (const auto* control = std::get_if<ControlRequest>(&prepared.value())) {
            if (std::holds_alternative<PlanReplacement>(*control)) return GroupKind::readonly;
            if (std::holds_alternative<DelegationRequest>(*control)) return GroupKind::task;
            return std::nullopt;
        }
        if (!verdict) return std::nullopt;
        const auto& tool = std::get<std::unique_ptr<PreparedTool>>(prepared.value());
        if (!parallel(*verdict, tool->intent())) return std::nullopt;
        return GroupKind::readonly;
    };

    // 结果按 tool_calls 的原始顺序写入历史、写记录、发事件；能提交的前缀尽早提交（docs/design/agent.md §6）。
    const auto commit = [&] {
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
    };

    const auto make_on_output = [&](const ToolCall& call) {
        return [&sink, id = call.id](std::string_view chunk) { sink(ToolOutput{id, std::string(chunk)}); };
    };

    // 运行中网络判定：先查配置/会话规则；需要时走现有审批器；同一执行内同目标只问一次。
    // Dispatcher consumes requests from the execution mailbox and owns approval/journal calls.
    const auto make_network_decider = [&](std::size_t index) {
        auto decided = std::make_shared<std::map<std::string, std::pair<std::uint64_t, NetworkAction>>>();
        return [&, index, decided](const NetworkTarget& target, std::string& reason, std::stop_token execution_stop) -> NetworkAction {
            const std::string key = target.host + ":" + std::to_string(target.port);
            if (const auto found = decided->find(key); found != decided->end() && found->second.first == policy.revision())
                return found->second.second;
            const auto remember = [&](NetworkAction action) {
                decided->insert_or_assign(key, std::pair{policy.revision(), action});
                return action;
            };
            const Policy::NetworkDecision verdict = policy.check_network(target);
            if (verdict.kind == Policy::NetworkDecision::Kind::allow) return remember(NetworkAction::allow);
            if (verdict.kind == Policy::NetworkDecision::Kind::deny) {
                reason = verdict.reason;
                return remember(NetworkAction::deny);
            }
            if (!services_.approver) {
                reason = "runtime network approval is unavailable in this run";
                return remember(NetworkAction::deny);
            }
            Slot& slot = slots[index];
            Approval approval;
            approval.partially_executed = true;
            approval.call_id = slot.call->id;
            approval.tool = slot.call->name;
            approval.reason = std::format("A sandboxed command wants to connect to {}", key);
            approval.session_rule = std::format("Allow connections to {} for this session", key);
            approval.cwd = session_.config().cwd.string();
            approval.mode = policy.planning() ? "plan" : std::string(to_string(policy.mode()));
            if (const auto* tool = std::get_if<std::unique_ptr<PreparedTool>>(&slot.action.value()))
                approval.intent = (*tool)->intent();
            approval.requests.push_back({Approval::Request::Kind::network, key, "runtime network request"});
            const std::uint64_t before = policy.revision();
            const Decision decision = services_.approver(approval, execution_stop);
            if (execution_stop.stop_requested()) {
                reason = "cancelled while waiting for network approval";
                return remember(NetworkAction::cancel);
            }
            committer.commit_permission(approval, decision);
            if (decision.answer == Decision::Answer::allow || decision.answer == Decision::Answer::allow_session) {
                // 审批答复后重新核对：有效范围在等待期间变化且不再放行时不消费旧批准。
                if (policy.revision() != before && policy.check_network(target).kind !=
                                                       Policy::NetworkDecision::Kind::allow) {
                    reason = "permission state changed while waiting for network approval";
                    return remember(NetworkAction::cancel);
                }
                if (decision.answer == Decision::Answer::allow_session) policy.remember_network(target);
                return remember(NetworkAction::allow);
            }
            reason = decision.answer == Decision::Answer::deny_with_feedback ? decision.feedback
                                                                             : "the user denied network access";
            policy.remember_denied_network(target);
            return remember(NetworkAction::cancel);
        };
    };

    const auto authorize = [&](const ToolCall& call, const PreparedIntent& intent)
        -> std::expected<ExecutionGrant, ToolResult> {
        Verdict verdict = policy.evaluate(call, intent);
        if (verdict.kind == Verdict::Kind::deny)
            return std::unexpected(make_result(std::format(texts::kPolicyDenied, verdict.reason), true, false));
        if (verdict.kind == Verdict::Kind::allow) return verdict.grant;
        if (!services_.approver)
            return std::unexpected(make_result(std::format(texts::kApprovalUnavailable, verdict.approval.reason), true, false));
        const auto revision = policy.revision();
        const auto decision = services_.approver(verdict.approval, stop);
        committer.commit_permission(verdict.approval, decision);
        if (stop.stop_requested()) {
            outcome.stop = Outcome::Stop::interrupted;
            return std::unexpected(make_result(std::string(texts::kInterruptedCall), false, true));
        }
        if (decision.answer == Decision::Answer::deny) {
            outcome.stop = Outcome::Stop::denied;
            return std::unexpected(make_result(std::string(texts::kDenied), true, false));
        }
        if (decision.answer == Decision::Answer::deny_with_feedback)
            return std::unexpected(make_result(std::format(texts::kDeniedWithFeedback, decision.feedback), true, false));
        if (policy.revision() != revision) {
            auto current = policy.evaluate(call, intent);
            if (current.kind == Verdict::Kind::allow) return current.grant;
            return std::unexpected(make_result("Permission changed during approval; prepare the operation again.", true, false));
        }
        policy.remember(verdict.approval, decision);
        return policy.grant_for(verdict.approval, decision);
    };

    const auto execute = [&](Slot& slot, const ExecutionGrant& grant) {
        if (const auto* control_request = std::get_if<ControlRequest>(&slot.action.value()))
            return control.execute(*control_request, stop);
        ExecutionGrant scoped = grant;
        if (scoped.revision != policy.revision())
            return make_result("Permission changed before execution; prepare the operation again.", true, false);
        // Parallel read-only groups cannot request additional permissions.
        scoped.network_decider = {};
        return std::get<std::unique_ptr<PreparedTool>>(slot.action.value())
            ->execute(scoped, make_on_output(*slot.call), stop);
    };

    // 串行执行：普通工具写 tool_started 审计；ask/exit_plan 只发实时开始、不写记录。
    const auto run_serial = [&](std::size_t i, const ExecutionGrant& grant) {
        Slot& slot = slots[i];
        const bool ordinary = std::holds_alternative<std::unique_ptr<PreparedTool>>(slot.action.value());
        const ToolStarted started{slot.call->id, slot.call->name, slot.summary, grant};
        if (ordinary && grant.revision != policy.revision()) {
            slot.result = make_result("Permission changed before execution; prepare the operation again.", true, false);
            return;
        }
        if (ordinary) committer.commit_tool_started(started);
        sink(started);
        if (!ordinary) {
            slot.result = execute(slot, grant);
        } else {
            struct Request {
                NetworkTarget target;
                std::stop_token stop;
                std::promise<std::pair<NetworkAction, std::string>> reply;
            };
            struct Mailbox {
                std::mutex mutex;
                std::condition_variable changed;
                std::deque<std::shared_ptr<Request>> requests;
                bool done = false;
            } mailbox;
            ExecutionGrant scoped = grant;
            scoped.network_decider = [&](const NetworkTarget& target, std::string& reason, std::stop_token execution_stop) {
                auto request = std::make_shared<Request>();
                request->target = target;
                request->stop = execution_stop;
                auto reply = request->reply.get_future();
                {
                    const std::lock_guard lock(mailbox.mutex);
                    mailbox.requests.push_back(request);
                }
                mailbox.changed.notify_one();
                auto [action, detail] = reply.get();
                reason = std::move(detail);
                return action;
            };
            auto decide = make_network_decider(i);
            std::jthread worker([&] {
                slot.result = std::get<std::unique_ptr<PreparedTool>>(*slot.action)
                    ->execute(scoped, make_on_output(*slot.call), stop);
                {
                    const std::lock_guard lock(mailbox.mutex);
                    mailbox.done = true;
                }
                mailbox.changed.notify_one();
            });
            for (;;) {
                std::unique_lock lock(mailbox.mutex);
                mailbox.changed.wait(lock, [&] { return mailbox.done || !mailbox.requests.empty(); });
                if (mailbox.requests.empty()) break;
                auto request = mailbox.requests.front();
                mailbox.requests.pop_front();
                lock.unlock();
                std::string reason;
                NetworkAction action = NetworkAction::cancel;
                try { action = decide(request->target, reason, request->stop); }
                catch (const std::exception& error) { reason = error.what(); }
                request->reply.set_value({action, std::move(reason)});
            }
        }
        if (!ordinary && slot.result->interrupted) outcome.stop = Outcome::Stop::interrupted;
    };

    // 并行组：ToolStarted 按顺序在 agent 线程上发；执行分块、每块至多 width 个 jthread，
    // 块内 join 完才起下一块；每个线程只写自己的 slot（docs/design/agent.md §6）。
    const auto run_group = [&] {
        if (group.empty()) return;
        const std::size_t width = group_kind == GroupKind::task
                                      ? static_cast<std::size_t>(
                                            session_.config().options.run.max_parallel_tasks)
                                      : static_cast<std::size_t>(session_.config().options.run.max_parallel_tools);
        for (const Pending& p : group) {
            Slot& slot = slots[p.slot];
            const ToolStarted started{slot.call->id, slot.call->name, slot.summary, p.grant};
            committer.commit_tool_started(started);
            sink(started);
        }
        for (std::size_t base = 0; base < group.size(); base += width) {
            const std::size_t end = std::min(base + width, group.size());
            std::vector<std::jthread> threads;
            threads.reserve(end - base);
            for (std::size_t k = base; k < end; ++k) {
                Pending& p = group[k];
                Slot& slot = slots[p.slot];
                threads.emplace_back([&, grant = p.grant] {
                    slot.result = execute(slot, grant);
                });
            }
        } // jthread 析构时 join：这一块全部结束才开始下一块
        group.clear();
    };

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

} // namespace dagent::agent
