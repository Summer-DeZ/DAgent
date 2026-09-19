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

constexpr std::size_t kGroupWidth = 8; ///< 并行组每块的线程数上限（README §5）

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
                                       const Approver& approver, std::stop_token stop) {
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

    // 结果按 tool_calls 的原始顺序写入历史、写记录、发事件；能提交的前缀尽早提交（05-dispatch §4.3）。
    const auto commit = [&] {
        while (committed < slots.size() && slots[committed].result.has_value()) {
            Slot& slot = slots[committed];
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

    // 串行执行：agent 线程上跑（05-dispatch §4.2 run_serial）。
    const auto run_serial = [&](std::size_t i, tools::Call& call, const tools::Grant& grant) {
        Slot& slot = slots[i];
        sink(ToolStarted{slot.call->id, slot.call->name, slot.summary, grant});
        slot.result = call.run(grant, make_on_output(*slot.call), stop);
    };

    // 并行组：ToolStarted 按顺序在 agent 线程上发；执行分块、每块至多 8 个 jthread，
    // 块内 join 完才起下一块；每个线程只写自己的 slot（05-dispatch §4.4）。
    const auto run_group = [&] {
        if (group.empty()) return;
        for (const Pending& p : group) {
            const Slot& slot = slots[p.slot];
            sink(ToolStarted{slot.call->id, slot.call->name, slot.summary, p.grant});
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
            slot.result = make_result(std::format(texts::kToolLimit, budget), true, false);
            outcome.hit_limit = true;
            continue;
        }
        ++outcome.handled;

        const tools::Tool* tool = registry_.find(slot.call->name);
        if (tool == nullptr) {
            // 未知工具没有 prepare，不触发跑组（05-dispatch §5）。
            slot.result = make_result(
                std::format(texts::kUnknownTool, slot.call->name, available_tools(registry_)), true, false);
            commit();
            continue;
        }

        auto prepared = tool->prepare(slot.call->arguments, tool_ctx_);
        std::optional<Verdict> verdict;
        if (prepared) verdict = policy_.evaluate(*slot.call, prepared.value()->intent());
        // 这个调用进不了并行组：先跑完挂起的组，再重新 prepare（prepare 无副作用，05-dispatch 规则 3）。
        if (!group.empty() && !(prepared && parallel(*verdict, prepared.value()->intent()))) {
            run_group();
            prepared = tool->prepare(slot.call->arguments, tool_ctx_);
            verdict.reset();
            if (prepared) verdict = policy_.evaluate(*slot.call, prepared.value()->intent());
        }
        if (!prepared) {
            slot.result = std::move(prepared.error());
            commit();
            continue;
        }
        slot.summary = prepared.value()->intent().summary; // 以最后一次 prepare 的 Intent 为准
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
                log_agent()->warn("需要询问但没有 Approver（编程错误）：{}", slot.call->name);
                slot.result =
                    make_result(std::format(texts::kPolicyDenied, approval.reason), true, false);
                break;
            }
            const Decision decision = approver(approval, stop);
            if (stop.stop_requested()) {
                outcome.stop = DispatchOutcome::Stop::interrupted;
                slot.result = make_result(std::string(texts::kInterruptedCall), false, true);
                break;
            }
            recorder_.permission(slot.call->id, decision, approval.session_rule);
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
