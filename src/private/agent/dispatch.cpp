#include <cassert>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agent/agent.hpp"
#include "base/log.hpp"

namespace dagent::agent {
namespace {

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
    std::vector<Slot> slots(calls.size());
    for (std::size_t i = 0; i < calls.size(); ++i) {
        slots[i].call = &calls[i];
        slots[i].summary = fallback_summary(calls[i]);
    }

    DispatchOutcome outcome;
    std::size_t committed = 0;

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

    const auto run_call = [&](tools::Call& call, Slot& slot, const tools::Grant& grant,
                              std::stop_token call_stop) {
        sink(ToolStarted{slot.call->id, slot.call->name, slot.summary, grant});
        slot.result = call.run(
            grant,
            [&](std::string_view chunk) { sink(ToolOutput{slot.call->id, std::string(chunk)}); },
            call_stop);
    };

    for (Slot& slot : slots) {
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
            slot.result = make_result(
                std::format(texts::kUnknownTool, slot.call->name, available_tools(registry_)), true, false);
            commit();
            continue;
        }

        auto prepared = tool->prepare(slot.call->arguments, tool_ctx_);
        if (!prepared) {
            slot.result = std::move(prepared.error());
            commit();
            continue;
        }
        slot.summary = prepared.value()->intent().summary;
        const Verdict verdict = policy_.evaluate(*slot.call, prepared.value()->intent());
        log_agent()->debug("工具调用 {}：{} → {}", slot.call->name, slot.summary,
                           verdict.kind == Verdict::Kind::allow    ? "allow"
                           : verdict.kind == Verdict::Kind::ask    ? "ask"
                                                                   : "deny");

        switch (verdict.kind) {
        case Verdict::Kind::allow:
            run_call(*prepared.value(), slot, verdict.grant, stop);
            break;
        case Verdict::Kind::deny:
            slot.result = make_result(std::format(texts::kPolicyDenied, verdict.reason), true, false);
            break;
        case Verdict::Kind::ask: {
            const Approval& approval = verdict.approval;
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
                run_call(*prepared.value(), slot, policy_.grant_for(approval, decision), stop);
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
    commit();
    // 最后一个调用执行中被中断时，循环里没有下一个调用来发现 stop。
    if (outcome.stop == DispatchOutcome::Stop::none && stop.stop_requested()) {
        outcome.stop = DispatchOutcome::Stop::interrupted;
    }
    assert(committed == slots.size());
    return outcome;
}

} // namespace dagent::agent
