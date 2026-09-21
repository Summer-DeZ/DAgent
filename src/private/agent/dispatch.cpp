#include <algorithm>
#include <cassert>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "agent/agent.hpp"
#include "base/log.hpp"

namespace dagent::agent {
namespace {

constexpr std::size_t kGroupWidth = 8; ///< 并行组每块的线程数上限（docs/design/agent.md §1）

std::shared_ptr<spdlog::logger> log_agent() { return base::logger("agent"); }

tools::Result make_result(std::string text, bool is_error, bool interrupted) {
    tools::Result result;
    result.text = std::move(text);
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

std::string available_tools(const tools::Registry& registry) {
    std::string names;
    for (const tools::Spec* spec : registry.specs()) {
        if (!names.empty()) names += "、";
        names += spec->name;
    }
    return names;
}

} // namespace

Agent::DispatchOutcome Agent::dispatch(const std::vector<ToolCall>& calls, int budget, const Sink& sink,
                                       const Approver& approver, const Asker& asker,
                                       std::stop_token stop) {
    struct Slot {
        const ToolCall* call = nullptr;
        std::string summary;
        std::optional<tools::Result> result; ///< 有值 = 可以提交
    };
    struct Pending {
        std::size_t slot;
        std::unique_ptr<tools::Call> call;
        tools::Grant grant;
    };

    std::vector<Slot> slots(calls.size());
    for (std::size_t i = 0; i < calls.size(); ++i) {
        slots[i].call = &calls[i];
        slots[i].summary = fallback_summary(calls[i]);
    }

    DispatchOutcome outcome;
    std::size_t committed = 0;
    std::vector<Pending> group; ///< 挂起的并行组

    // 结果按 tool_calls 的原始顺序写入历史、写记录、发事件；能提交的前缀尽早提交（docs/design/agent.md §6）。
    const auto commit = [&] {
        while (committed < slots.size() && slots[committed].result.has_value()) {
            Slot& slot = slots[committed];
            if (const auto* view = std::get_if<tools::McpView>(&slot.result->display);
                view && view->disconnected) {
                // 告诉模型这个 server 会重连还是已不可用（T12 / T13），免得它在工具消失后反复寻找。
                slot.result->text += hub_.mark_disconnected(view->server, slot.result->text);
            }
            const std::int64_t ordinal =
                conversation_.add_tool_result(slot.call->id, slot.result->text, slot.summary);
            recorder_.tool(ordinal, *slot.call, slot.summary, *slot.result);
            check_broken(sink);
            sink(ToolFinished{slot.call->id, slot.call->name, slot.summary, *slot.result});
            ++committed;
        }
    };

    const auto make_on_output = [&](const ToolCall& call) {
        return [&sink, id = call.id](std::string_view chunk) { sink(ToolOutput{id, std::string(chunk)}); };
    };

    // 串行执行：agent 线程上跑（docs/design/agent.md §6）。
    const auto run_serial = [&](std::size_t i, tools::Call& call, const tools::Grant& grant) {
        Slot& slot = slots[i];
        const ToolStarted started{slot.call->id, slot.call->name, slot.summary, grant};
        recorder_.tool_started(started);
        check_broken(sink);
        sink(started);
        slot.result = call.run(grant, make_on_output(*slot.call), stop);
    };

    // 并行组：ToolStarted 按顺序在 agent 线程上发；执行分块、每块至多 8 个 jthread，
    // 块内 join 完才起下一块；每个线程只写自己的 slot（docs/design/agent.md §6）。
    const auto run_group = [&] {
        if (group.empty()) return;
        for (const Pending& p : group) {
            const Slot& slot = slots[p.slot];
            const ToolStarted started{slot.call->id, slot.call->name, slot.summary, p.grant};
            recorder_.tool_started(started);
            check_broken(sink);
            sink(started);
        }
        for (std::size_t base = 0; base < group.size(); base += kGroupWidth) {
            const std::size_t end = std::min(base + kGroupWidth, group.size());
            std::vector<std::jthread> threads;
            threads.reserve(end - base);
            for (std::size_t k = base; k < end; ++k) {
                Pending& p = group[k];
                Slot& slot = slots[p.slot];
                tools::Call* call = p.call.get();
                threads.emplace_back([&, call, grant = p.grant] {
                    slot.result = call->run(grant, make_on_output(*slot.call), stop);
                });
            }
        } // jthread 析构时 join：这一块全部结束才开始下一块
        group.clear();
    };

    for (std::size_t i = 0; i < slots.size(); ++i) {
        Slot& slot = slots[i];
        if (outcome.stop == DispatchOutcome::Stop::interrupted || stop.stop_requested()) {
            outcome.stop = DispatchOutcome::Stop::interrupted;
            slot.result = make_result(std::string(texts::kInterruptedCall), false, true);
            continue;
        }
        if (outcome.stop == DispatchOutcome::Stop::denied) {
            slot.result = make_result(std::string(texts::kPriorDenied), true, false);
            continue;
        }
        if (outcome.handled == budget) {
            slot.result = make_result(std::format(texts::kToolLimit, setup_.options.run.max_tool_calls), true, false);
            outcome.hit_limit = true;
            continue;
        }
        ++outcome.handled;

        const tools::Tool* tool = registry_.find(slot.call->name);
        if (tool == nullptr) {
            // 未知工具没有 prepare，不触发跑组（docs/design/agent.md §6）。
            slot.result = make_result(
                std::format(texts::kUnknownTool, slot.call->name, available_tools(registry_)), true, false);
            commit();
            continue;
        }

        auto prepared = tool->prepare(slot.call->arguments, tool_ctx_);
        std::optional<Verdict> verdict;
        const auto interactive = [&] {
            if (!prepared) return false;
            const auto kind = prepared.value()->intent().kind;
            return kind == tools::Intent::Kind::ask || kind == tools::Intent::Kind::exit_plan;
        };
        if (prepared && !interactive()) verdict = policy_.evaluate(*slot.call, prepared.value()->intent());
        // 这个调用进不了并行组：先跑完挂起的组，再重新 prepare（prepare 无副作用，docs/design/agent.md §6）。
        if (!group.empty() && !(prepared && verdict && parallel(*verdict, prepared.value()->intent()))) {
            run_group();
            prepared = tool->prepare(slot.call->arguments, tool_ctx_);
            verdict.reset();
            if (prepared && prepared.value()->intent().kind != tools::Intent::Kind::ask &&
                prepared.value()->intent().kind != tools::Intent::Kind::exit_plan)
                verdict = policy_.evaluate(*slot.call, prepared.value()->intent());
        }
        if (!prepared) {
            slot.result = std::move(prepared.error());
            commit();
            continue;
        }
        slot.summary = prepared.value()->intent().summary; // 以最后一次 prepare 的 Intent 为准

        if (prepared.value()->intent().kind == tools::Intent::Kind::ask) {
            sink(ToolStarted{slot.call->id, slot.call->name, slot.summary, {}});
            tools::AskView view = prepared.value()->intent().ask;
            if (++questions_this_turn_ > 3) {
                slot.result = make_result(
                    "Question limit reached for this turn. Make the most reasonable choice, state the assumption, and continue.",
                    true, false);
                slot.result->display = view;
            } else if (!asker) {
                slot.result = make_result(
                    "Non-interactive run: cannot ask the user. Pick the most reasonable option, state the assumption you made, and continue.",
                    true, false);
                slot.result->display = view;
            } else {
                Question question;
                question.call_id = slot.call->id;
                question.header = view.header;
                question.prompt = view.prompt;
                question.multi_select = view.multi_select;
                question.allow_other = view.allow_other;
                for (const auto& option : view.options)
                    question.options.push_back({option.label, option.description});
                const Answer answer = asker(question, stop);
                view.selected = answer.selected;
                view.other = answer.other;
                view.cancelled = answer.cancelled;
                if (answer.cancelled || stop.stop_requested()) {
                    outcome.stop = DispatchOutcome::Stop::interrupted;
                    slot.result = make_result("The user cancelled the question.", false, true);
                } else {
                    std::string choices;
                    for (const int index : answer.selected) {
                        if (index < 0 || static_cast<std::size_t>(index) >= view.options.size()) continue;
                        if (!choices.empty()) choices += ", ";
                        choices += view.options[static_cast<std::size_t>(index)].label;
                    }
                    if (!answer.other.empty()) {
                        if (!choices.empty()) choices += ", ";
                        choices += answer.other;
                    }
                    slot.result = make_result("User chose: " + choices, false, false);
                }
                slot.result->display = std::move(view);
            }
            commit();
            continue;
        }

        if (prepared.value()->intent().kind == tools::Intent::Kind::exit_plan) {
            sink(ToolStarted{slot.call->id, slot.call->name, slot.summary, {}});
            tools::AskView view = prepared.value()->intent().ask;
            if (!policy_.planning()) {
                slot.result = make_result("exit_plan is only available while planning.", true, false);
            } else if (!asker) {
                slot.result = make_result(
                    "Non-interactive planning run: the plan cannot be confirmed. Present the final plan and stop without making changes.",
                    true, false);
                slot.result->display = view;
            } else {
                Question question;
                question.call_id = slot.call->id;
                question.header = view.header;
                question.prompt = view.prompt;
                question.allow_other = false;
                for (const auto& option : view.options)
                    question.options.push_back({option.label, option.description});
                const Answer answer = asker(question, stop);
                view.selected = answer.selected;
                view.cancelled = answer.cancelled;
                if (answer.cancelled || stop.stop_requested()) {
                    outcome.stop = DispatchOutcome::Stop::interrupted;
                    slot.result = make_result("Plan confirmation was cancelled.", false, true);
                } else if (answer.selected.empty() || answer.selected.front() == 2) {
                    slot.result = make_result("Continue planning. Refine the proposal and submit it again when ready.", false, false);
                } else {
                    const PermissionMode next = answer.selected.front() == 0
                                                    ? PermissionMode::workspace
                                                    : PermissionMode::ask;
                    policy_.set_mode(next);
                    policy_.set_planning(false);
                    policy_.set_read_only(setup_.read_only);
                    sink(ModeChanged{std::string(to_string(next)), false});
                    slot.result = make_result("Plan accepted. Begin implementation now.", false, false);
                }
                slot.result->display = std::move(view);
            }
            commit();
            continue;
        }

        const Verdict& v = *verdict;
        log_agent()->debug("工具调用 {}：{} → {}", slot.call->name, slot.summary,
                           v.kind == Verdict::Kind::allow    ? "allow"
                           : v.kind == Verdict::Kind::ask    ? "ask"
                                                             : "deny");

        switch (v.kind) {
        case Verdict::Kind::allow: {
            const tools::Intent& intent = prepared.value()->intent();
            if (parallel(v, intent)) {
                group.push_back(Pending{i, std::move(prepared.value()), v.grant});
            } else {
                run_serial(i, *prepared.value(), v.grant);
            }
            break;
        }
        case Verdict::Kind::deny:
            slot.result = make_result(std::format(texts::kPolicyDenied, v.reason), true, false);
            break;
        case Verdict::Kind::ask: {
            const Approval& approval = v.approval;
            if (!approver) {
                log_agent()->warn("需要用户批准，但当前运行方式没有交互审批器：{}", slot.call->name);
                slot.result =
                    make_result(std::format(texts::kApprovalUnavailable, approval.reason), true, false);
                break;
            }
            const Decision decision = approver(approval, stop);
            if (stop.stop_requested()) {
                outcome.stop = DispatchOutcome::Stop::interrupted;
                slot.result = make_result(std::string(texts::kInterruptedCall), false, true);
                break;
            }
            recorder_.permission(approval, decision);
            check_broken(sink);
            switch (decision.answer) {
            case Decision::Answer::allow:
            case Decision::Answer::allow_session:
                if (decision.answer == Decision::Answer::allow_session) {
                    policy_.remember(approval, decision);
                }
                run_serial(i, *prepared.value(), policy_.grant_for(approval, decision));
                break;
            case Decision::Answer::deny:
                slot.result = make_result(std::string(texts::kDenied), true, false);
                outcome.stop = DispatchOutcome::Stop::denied;
                break;
            case Decision::Answer::deny_with_feedback:
                slot.result =
                    make_result(std::format(texts::kDeniedWithFeedback, decision.feedback), true, false);
                break;
            }
            break;
        }
        }
        commit();
    }
    run_group();
    commit();
    // 最后一个调用执行中被中断时，循环里没有下一个调用来发现 stop。
    if (outcome.stop == DispatchOutcome::Stop::none && stop.stop_requested()) {
        outcome.stop = DispatchOutcome::Stop::interrupted;
    }
    assert(committed == slots.size());
    return outcome;
}

} // namespace dagent::agent
