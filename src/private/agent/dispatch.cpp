#include "agent/dispatch.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
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
#include "agent/decision_review.hpp"
#include "agent/permission.hpp"
#include "agent/parent_review.hpp"
#include "base/log.hpp"
#include "base/text.hpp"

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

} // namespace

ActionDispatcher::Outcome ActionDispatcher::dispatch(const std::vector<ToolCall>& calls, int budget) {
    SessionCommitter& committer = session_.committer();
    Policy& policy = session_.policy();
    ExecutionRegistry& executions = session_.executions();
    ControlActionExecutor& control = session_.control();
    ActionCatalog& catalog = session_.catalog();
    SessionResources* resources = services_.resources;
    const Sink& sink = services_.sink;
    const std::stop_token stop = services_.stop;
    ExecutionMailbox mailbox;
    ParentReviewer reviewer(session_, run_, services_);
    const Approver parent_reviewer = [&](Approval& approval, std::stop_token child_stop) {
        return reviewer.submit(mailbox, approval, child_stop);
    };
    control.set_parent_reviewer(
        session_.config().options.approval.mode != ParentReviewMode::disabled ? &parent_reviewer : nullptr);
    struct ResetReviewer {
        ControlActionExecutor& control;
        ~ResetReviewer() { control.set_parent_reviewer(nullptr); }
    } reset_reviewer{control};
    std::map<std::string, std::string> execution_ids;
    static std::atomic<std::uint64_t> sequence{0};
    const auto execution_id = [&](const std::string& call_id) -> const std::string& {
        const auto [execution, inserted] = execution_ids.try_emplace(call_id);
        if (inserted) execution->second = session_.meta().id + "/execution/" + std::to_string(++sequence);
        return execution->second;
    };
    const auto identify = [&](Approval& approval) {
        approval.identity.request_id = session_.meta().id + "/approval/" + std::to_string(++sequence);
        approval.identity.child_session_id = session_.meta().id;
        approval.identity.call_id = approval.call_id;
        approval.identity.execution_id = execution_id(approval.call_id);
        const auto live = policy.effective();
        approval.read_only = live.read_only;
        approval.planning = live.planning;
        for (const auto& grant : policy.session_grants()) approval.existing_permissions.push_back(grant.description);
    };
    const auto validate_decision = [&](const Approval& approval, Decision& decision, std::stop_token pending_stop) {
        const bool approved = decision.answer == Decision::Answer::allow || decision.answer == Decision::Answer::allow_session;
        if (pending_stop.stop_requested() || (approval.authority == ApprovalAuthority::parent_model &&
                                              approval.authority_stop.stop_requested())) {
            decision.answer = Decision::Answer::deny_with_feedback;
            decision.feedback = "Permission request cancelled or parent authority revoked; prepare again.";
            decision.state = "cancelled";
        } else if (approved && (policy.revision() != approval.identity.child_revision ||
                                !policy.valid_approval(approval, decision))) {
            decision.answer = Decision::Answer::deny_with_feedback;
            decision.feedback = "Permission changed or the requested approval scope is invalid; prepare again.";
            decision.state = "cancelled";
        } else if (decision.state.empty()) decision.state = approved ? "approved" : "denied";
    };
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

    // 权限变化终止执行后，用说明替换"被用户中断"，避免把系统降权误报成用户取消。
    const auto finish_execution = [&](Slot& slot) {
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
    };

    // 决策审阅：只有配置了决策模型、且本轮自动审批未被关闭时才尝试代批（计划 2.2/2.9）。
    const std::shared_ptr<DecisionServices>& decisions = session_.decision();
    const bool decision_ready = decisions != nullptr && decisions->enabled();
    std::optional<DecisionReviewer> decision_reviewer;
    const ReviewContext review_context = session_.review_context();
    if (decision_ready) decision_reviewer.emplace(*decisions, review_context, root_turn_id_);
    const auto gate_open = [&] {
        return services_.review_gate == nullptr || services_.review_gate->enabled();
    };
    /// 决策模型可用于本会话：只在 auto-edit（workspace、无只读/规划上限）下代批；
    /// unrestricted、ask、只读/规划保持原有语义（计划 §2.3/§3）。
    /// 子会话 may_ask=false（无人工出口）时也不能借它新增权限。
    const auto decision_allowed = [&] {
        if (decisions == nullptr) return false;
        if (!session_.is_main() && !services_.approver) return false;
        const EffectivePermission live = policy.effective();
        return live.mode == PermissionMode::workspace && !live.read_only && !live.planning;
    };
    const auto review_deadline = [&] {
        return std::chrono::steady_clock::now() +
               (decisions != nullptr ? decisions->options.timeout : std::chrono::milliseconds{10000});
    };
    const auto commit_auto_review = [&](const std::string& call_id, std::vector<std::string> review_ids,
                                        std::string_view result, std::string reason) {
        AutoReview event;
        event.call_id = call_id;
        event.review_ids = std::move(review_ids);
        event.result = std::string(result);
        event.reason = std::move(reason);
        committer.commit_auto_review(event);
    };
    /// 人工最终决定回填：只有用户明确批准/拒绝才更新动态阈值；父复批与取消不调整。
    const auto commit_feedback = [&](const Approval& approval, const Decision& decision) {
        if (!decision_ready || decisions->store == nullptr) return;
        if (approval.authority != ApprovalAuthority::user) return;
        if (decision.state != "approved" &&
            !(decision.state == "denied" && decision.explicit_denial)) return;
        ReviewFeedback feedback;
        feedback.approval_request_id = approval.identity.request_id;
        feedback.review_ids = approval.review_ids;
        feedback.final_authority = approval.authority;
        feedback.user_decided = true;
        feedback.approved = decision.answer == Decision::Answer::allow ||
                            decision.answer == Decision::Answer::allow_session;
        try {
            decisions->store->finish(feedback);
        } catch (const std::exception& error) {
            log_agent()->warn("决策审阅反馈保存失败：{}", error.what());
        }
    };

    // 运行中网络判定：先查配置/会话拒绝；需要时做脚本强制复审与 d3 代批；再走现有审批器。
    // Dispatcher consumes requests from the execution mailbox and owns approval/journal calls.
    const auto make_network_decider = [&](std::size_t index) {
        auto decided = std::make_shared<std::map<std::string, std::pair<std::uint64_t, NetworkAction>>>();
        return [&, index, decided](const NetworkTarget& target, std::string& reason, std::stop_token execution_stop) -> NetworkAction {
            if (execution_stop.stop_requested() || stop.stop_requested()) {
                reason = "execution cancelled before network authorization";
                return NetworkAction::cancel;
            }
            const std::string key = target.host + ":" + std::to_string(target.port);
            const auto cancel = [&] {
                if (!stop.stop_requested() && slots[index].execution != nullptr)
                    slots[index].execution->note_termination(reason);
                return NetworkAction::cancel;
            };
            const auto request_revision = policy.revision();
            if (const auto found = decided->find(key); found != decided->end() && found->second.first == request_revision)
                return found->second.second;
            const auto remember = [&](NetworkAction action, bool persisted = false) {
                decided->insert_or_assign(key, std::pair{request_revision, action});
                // persisted 表示批准来自配置/会话规则，撤销这类规则时要能终止使用它的执行。
                if (action == NetworkAction::allow && slots[index].execution != nullptr)
                    slots[index].execution->note_network(target, persisted);
                if (action == NetworkAction::cancel) return cancel();
                return action;
            };

            const Policy::NetworkRules rules = policy.inspect_network(target);
            if (policy.revision() != request_revision) {
                reason = "permission changed during network authorization";
                return cancel();
            }
            if (rules.match == Policy::NetworkRules::Match::denied) {
                reason = rules.reason;
                return remember(NetworkAction::deny);
            }

            Slot& slot = slots[index];
            const auto* tool = std::get_if<std::unique_ptr<PreparedTool>>(&slot.action.value());
            const PreparedIntent empty_intent;
            const PreparedIntent& intent = tool != nullptr ? (*tool)->intent() : empty_intent;
            const CommandIntent* command = intent.command ? &*intent.command : nullptr;
            const bool has_targets = command != nullptr && !command->script_targets.empty();
            // 本轮自动审批关闭：跳过 d3 与父复批，直接交给人工（计划 2.1/2.9）。
            const bool review_enabled = decision_allowed() && gate_open() && decision_reviewer.has_value();
            std::vector<ScriptEvidence> evidence;
            bool must_review = false;
            if (decision_allowed() && has_targets) {
                for (const auto& script : command->script_targets) {
                    const auto relative = script.path.lexically_relative(session_.config().cwd);
                    PreparedIntent reading;
                    reading.kind = ToolKind::read;
                    reading.paths.push_back({script.path, Access::read,
                        !relative.empty() && !relative.is_absolute() && *relative.begin() != ".."});
                    if (policy.evaluate(*slot.call, reading).kind != Verdict::Kind::allow) {
                        ScriptEvidence missing;
                        missing.target = script.path;
                        missing.kind = ScriptEvidence::Kind::unavailable;
                        missing.reason = "script evidence requires file read approval";
                        evidence.push_back(std::move(missing));
                        must_review = true;
                        continue;
                    }
                    auto item = (*tool)->script_evidence(script, session_.config().session_started,
                                                         review_deadline(), execution_stop);
                    if (execution_stop.stop_requested() || stop.stop_requested()) return cancel();
                    if (!item) {
                        ScriptEvidence missing;
                        missing.target = script.path;
                        missing.kind = ScriptEvidence::Kind::unavailable;
                        missing.reason = item.error().detail;
                        evidence.push_back(std::move(missing));
                    } else evidence.push_back(std::move(*item));
                    must_review = must_review || evidence.back().kind != ScriptEvidence::Kind::unchanged;
                }
            }
            if (rules.match == Policy::NetworkRules::Match::allowed && !must_review)
                return remember(NetworkAction::allow, true);

            // 审批身份在审阅前生成：审阅记录携带同一个 request_id，供后续阈值反馈关联。
            Approval approval;
            approval.partially_executed = true;
            approval.call_id = slot.call->id;
            approval.tool = slot.call->name;
            approval.reason = std::format("{} wants to connect to {}", slot.call->name, key);
            approval.cwd = session_.config().cwd.string();
            approval.mode = policy.planning() ? "plan" : std::string(to_string(policy.mode()));
            approval.intent = intent;
            approval.requests.push_back({Approval::Request::Kind::network, key, "runtime network request"});
            approval.arguments = slot.call->arguments;
            identify(approval);
            approval.identity.child_revision = request_revision;
            bool approved_by_review = false;
            std::string review_reason;
            std::vector<std::string> review_ids;
            bool scripts_passed = true;

            if (review_enabled && has_targets) {
                // 本会话修改过的目标必须复审，不能因 host 已放行而跳过（计划 2.2/2.4）。
                const ReviewOperation operation = [&] {
                    ReviewOperation value;
                    value.identity = approval.identity;
                    value.tool = slot.call->name;
                    value.command = command->command;
                    value.arguments = slot.call->arguments;
                    value.network = target;
                    value.file_execution = true;
                    value.mixed_scripts = command->script_targets.size() > 1;
                    return value;
                }();
                for (std::size_t i = 0; i < command->script_targets.size(); ++i) {
                    const ScriptTarget& script = command->script_targets[i];
                    const ReviewOperation scoped = operation;
                    const ReviewResult result = decision_reviewer->review_script(scoped, script, evidence[i],
                                                                        review_deadline(), execution_stop);
                    review_ids.push_back(result.record_id);
                    if (result.disposition == ReviewDisposition::cancelled) {
                        reason = result.reason;
                        return cancel();
                    }
                    if (result.disposition == ReviewDisposition::approve) continue;
                    scripts_passed = false;
                    review_reason = result.reason.empty()
                                        ? "the script review did not approve this network request"
                                        : result.reason;
                }
                if (scripts_passed && !command->has_unresolved_execution) approved_by_review = true;
            }
            if (review_enabled && scripts_passed && !approved_by_review &&
                (!has_targets || (command != nullptr && command->has_unresolved_execution)) &&
                (has_targets || rules.match == Policy::NetworkRules::Match::unmatched)) {
                // 普通命令联网（或混合命令的其余联网动作）：按完整命令判断风险与授权。
                ReviewOperation operation;
                operation.identity = approval.identity;
                operation.tool = slot.call->name;
                operation.command = command != nullptr ? command->command : std::string{};
                operation.arguments = slot.call->arguments;
                operation.network = target;
                operation.file_execution = command != nullptr && command->has_unresolved_execution;
                const ReviewResult result = decision_reviewer->review_action(ReviewScene::command_network, operation,
                                                                    review_deadline(), execution_stop);
                review_ids.push_back(result.record_id);
                if (result.disposition == ReviewDisposition::cancelled) {
                    reason = result.reason;
                    return cancel();
                }
                if (result.disposition == ReviewDisposition::approve) approved_by_review = true;
                else if (review_reason.empty()) review_reason = result.reason;
            }

            if (approved_by_review) {
                if (!gate_open()) {
                    // 审阅期间整轮被关闭：结果不生效，转人工。
                    approved_by_review = false;
                    review_reason = "automatic approval was disabled for this turn; the request needs approval";
                } else {
                    approval.authority = ApprovalAuthority::decision_model;
                    approval.scope = Approval::Scope::network_target;
                    approval.review_ids = review_ids;
                    Decision decision;
                    decision.answer = Decision::Answer::allow;
                    decision.state = "approved";
                    decision.feedback = review_reason.empty() ? "auto-review approved this network request"
                                                              : review_reason;
                    if (!policy.consume_network_approval(target, approval, decision)) {
                        decision.answer = Decision::Answer::deny_with_feedback;
                        decision.state = "cancelled";
                        decision.feedback = "Permission changed before network authorization was consumed.";
                        committer.commit_permission(approval, decision);
                        reason = decision.feedback;
                        return remember(NetworkAction::cancel);
                    }
                    slot.execution->note_authority(approval.authority_stop);
                    ExecutionGrant network_grant = slot.execution->grant;
                    network_grant.authority = approval.authority;
                    network_grant.approval = approval.identity;
                    network_grant.source = GrantSource::once;
                    network_grant.revision = policy.revision();
                    network_grant.authority_stop = approval.authority_stop;
                    network_grant.network_targets.push_back(key);
                    committer.commit_permission(approval, decision, &network_grant);
                    commit_auto_review(slot.call->id, review_ids, "approved", decision.feedback);
                    if (execution_stop.stop_requested() || approval.authority_stop.stop_requested()) {
                        reason = "permission revoked before network approval was applied";
                        return cancel();
                    }
                    return remember(NetworkAction::allow);
                }
            } else if (review_enabled && !review_ids.empty()) {
                // d3 未批准：不写入会话拒绝规则，转后续审批。
                commit_auto_review(slot.call->id, review_ids, "needs_approval",
                                   review_reason.empty() ? "the decision model did not approve this request"
                                                         : review_reason);
            }

            if (!services_.approver) {
                reason = review_reason.empty() ? "runtime network approval is unavailable in this run" : review_reason;
                return remember(NetworkAction::deny);
            }

            approval.authority = ApprovalAuthority::user;
            approval.session_rule = std::format("Allow connections to {} for this session", key);
            approval.review_ids = review_ids;
            Decision decision = services_.approver(approval, execution_stop);
            validate_decision(approval, decision, execution_stop);
            if (decision.answer == Decision::Answer::allow || decision.answer == Decision::Answer::allow_session) {
                if (!policy.consume_network_approval(target, approval, decision)) {
                    decision.answer = Decision::Answer::deny_with_feedback;
                    decision.state = "cancelled";
                    decision.feedback = "Permission changed before network authorization was consumed.";
                    committer.commit_permission(approval, decision);
                    reason = decision.feedback;
                    return remember(NetworkAction::cancel);
                }
                slot.execution->note_authority(approval.authority_stop);
                ExecutionGrant network_grant = slot.execution->grant;
                network_grant.authority = approval.authority;
                network_grant.approval = approval.identity;
                network_grant.source = decision.answer == Decision::Answer::allow_session ? GrantSource::session : GrantSource::once;
                network_grant.revision = policy.revision();
                network_grant.authority_stop = approval.authority_stop;
                network_grant.network_targets.push_back(key);
                committer.commit_permission(approval, decision, &network_grant);
                commit_feedback(approval, decision);
                if (execution_stop.stop_requested() || approval.authority_stop.stop_requested()) {
                    reason = "permission revoked before network approval was applied";
                    return cancel();
                }
                return remember(NetworkAction::allow, decision.answer == Decision::Answer::allow_session);
            }
            committer.commit_permission(approval, decision);
            commit_feedback(approval, decision);
            reason = decision.answer == Decision::Answer::deny_with_feedback ? decision.feedback
                : approval.authority == ApprovalAuthority::parent_model ? "the parent agent denied network access"
                                                                         : "the user denied network access";
            if (decision.explicit_denial && decision.state == "denied")
                policy.remember_denied_network(target, approval.authority);
            return remember(NetworkAction::cancel);
        };
    };

    const auto authorize = [&](const ToolCall& call, const PreparedIntent& intent)
        -> std::expected<ExecutionGrant, ToolResult> {
        Verdict verdict = policy.evaluate(call, intent);
        if (verdict.kind == Verdict::Kind::deny)
            return std::unexpected(make_result(std::format(texts::kPolicyDenied, verdict.reason), true, false));
        if (verdict.kind == Verdict::Kind::allow) return verdict.grant;
        identify(verdict.approval);

        // 决策模型代批：只处理本计划列出的场景（工作区外普通文件读取、MCP 动作）。
        // 不代批的请求直接进入后续审批，不写入会话拒绝规则。
        if (decision_allowed() && gate_open() && decision_reviewer &&
            (verdict.approval.scope == Approval::Scope::exact_file_read ||
             (verdict.approval.scope == Approval::Scope::operation &&
              intent.kind == ToolKind::external))) {
            const bool outside_read = verdict.approval.scope == Approval::Scope::exact_file_read;
            std::vector<std::string> review_ids;
            std::string review_reason;
            bool all_approved = true;
            bool review_cancelled = false;
            if (outside_read) {
                for (const ResourceIntent& path : verdict.approval.intent.paths) {
                    ReviewOperation operation;
                    operation.identity = verdict.approval.identity;
                    operation.tool = call.name;
                    operation.command = intent.command ? intent.command->command : std::string{};
                    operation.arguments = call.arguments;
                    operation.path = path.path;
                    operation.file_execution = intent.command && intent.command->known_readonly;
                    const ReviewResult result = decision_reviewer->review_action(ReviewScene::outside_read, operation,
                                                                        review_deadline(), stop);
                    review_ids.push_back(result.record_id);
                    if (result.disposition == ReviewDisposition::approve) continue;
                    all_approved = false;
                    review_reason = result.reason;
                    review_cancelled = result.disposition == ReviewDisposition::cancelled;
                    break;
                }
            } else {
                ReviewOperation operation;
                operation.identity = verdict.approval.identity;
                operation.tool = call.name;
                operation.arguments = call.arguments;
                const ReviewResult result = decision_reviewer->review_action(ReviewScene::mcp_action, operation,
                                                                    review_deadline(), stop);
                review_ids.push_back(result.record_id);
                all_approved = result.disposition == ReviewDisposition::approve;
                review_reason = result.reason;
                review_cancelled = result.disposition == ReviewDisposition::cancelled;
            }
            if (review_cancelled || stop.stop_requested()) {
                commit_auto_review(call.id, review_ids, "cancelled",
                                   review_reason.empty() ? "the decision review was cancelled" : review_reason);
                outcome.stop = Outcome::Stop::interrupted;
                return std::unexpected(make_result(std::string(texts::kInterruptedCall), false, true));
            }
            if (all_approved && !gate_open()) {
                all_approved = false;
                review_reason = "automatic approval was disabled for this turn; the request needs approval";
            }
            if (all_approved) {
                verdict.approval.authority = ApprovalAuthority::decision_model;
                verdict.approval.scope = outside_read ? Approval::Scope::exact_file_read
                                                      : Approval::Scope::mcp_action;
                verdict.approval.review_ids = review_ids;
                Decision decision;
                // 工作区外普通文件可保存文件级会话授权；MCP 只放行这一次调用。
                decision.answer = outside_read ? Decision::Answer::allow_session : Decision::Answer::allow;
                decision.state = "approved";
                decision.feedback = review_reason.empty() ? "auto-review approved this request" : review_reason;
                std::optional<ExecutionGrant> grant = policy.consume_approval(verdict.approval, decision);
                if (!grant) {
                    decision.answer = Decision::Answer::deny_with_feedback;
                    decision.state = "cancelled";
                    decision.feedback = "Permission changed before authorization was consumed; prepare again.";
                    committer.commit_permission(verdict.approval, decision);
                    commit_auto_review(call.id, review_ids, "cancelled", decision.feedback);
                    return std::unexpected(make_result(decision.feedback, true, false));
                }
                grant->execution_id = execution_id(call.id);
                committer.commit_permission(verdict.approval, decision, &*grant);
                commit_auto_review(call.id, review_ids, "approved", decision.feedback);
                if (stop.stop_requested()) {
                    outcome.stop = Outcome::Stop::interrupted;
                    return std::unexpected(make_result(std::string(texts::kInterruptedCall), false, true));
                }
                return *grant;
            }
            commit_auto_review(call.id, review_ids, "needs_approval",
                               review_reason.empty() ? "the decision model did not approve this request"
                                                     : review_reason);
            // 切换到后续审批时保留前序审阅编号（计划 §8.1）。
            verdict.approval.review_ids = review_ids;
        }

        if (!services_.approver)
            return std::unexpected(make_result(std::format(texts::kApprovalUnavailable, verdict.approval.reason), true, false));
        auto decision = services_.approver(verdict.approval, stop);
        validate_decision(verdict.approval, decision, stop);
        std::optional<ExecutionGrant> grant;
        if (decision.answer == Decision::Answer::allow || decision.answer == Decision::Answer::allow_session) {
            grant = policy.consume_approval(verdict.approval, decision);
            if (grant) grant->execution_id = execution_id(call.id);
            else {
                decision.answer = Decision::Answer::deny_with_feedback;
                decision.state = "cancelled";
                decision.feedback = "Permission changed before authorization was consumed; prepare again.";
            }
        }
        committer.commit_permission(verdict.approval, decision, grant ? &*grant : nullptr);
        commit_feedback(verdict.approval, decision);
        if (stop.stop_requested()) {
            outcome.stop = Outcome::Stop::interrupted;
            return std::unexpected(make_result(std::string(texts::kInterruptedCall), false, true));
        }
        if (decision.answer == Decision::Answer::deny) {
            outcome.stop = Outcome::Stop::denied;
            return std::unexpected(make_result(std::string(texts::kDenied), true, false));
        }
        if (!grant) {
            const std::string feedback = verdict.approval.authority == ApprovalAuthority::parent_model
                ? "Parent permission review " + decision.state + ": " + decision.feedback
                : "Permission request denied: " + decision.feedback;
            return std::unexpected(make_result(feedback, true, false));
        }
        return *grant;
    };

    // web_fetch 逐跳 URL 审阅与结果扫描：审阅与扫描都走 execution mailbox（在执行线程上编排）。
    const auto make_web_request_decider = [&](std::size_t index) {
        return [&, index](const WebRequestIntent& request, std::stop_token execution_stop) -> WebRequestAccess {
            WebRequestAccess access;
            access.action = NetworkAction::deny;
            if (execution_stop.stop_requested() || stop.stop_requested()) {
                access.action = NetworkAction::cancel;
                access.reason = "execution cancelled before web request authorization";
                return access;
            }
            const std::string key = request.target.host + ":" + std::to_string(request.target.port);
            const auto request_revision = policy.revision();
            const auto hop_decider = [&, index](WebRequestIntent reviewed, std::uint64_t revision,
                                               bool persisted, std::stop_token authority_stop) {
                return [&, index, reviewed, revision, persisted, authority_stop](const NetworkTarget& target,
                    std::string& reason, std::stop_token token) -> NetworkAction {
                    if (token.stop_requested() || stop.stop_requested() || authority_stop.stop_requested() ||
                        policy.revision() != revision) {
                        reason = "permission changed or execution cancelled before the web request was sent";
                        return NetworkAction::cancel;
                    }
                    if (target.host != reviewed.target.host || target.port != reviewed.target.port) {
                        reason = "the connection target does not match the reviewed URL";
                        return NetworkAction::cancel;
                    }
                    if (policy.inspect_network(target).match == Policy::NetworkRules::Match::denied) {
                        reason = "network target is denied";
                        return NetworkAction::deny;
                    }
                    slots[index].execution->note_network(target, persisted);
                    return NetworkAction::allow;
                };
            };
            const Policy::NetworkRules rules = policy.inspect_network(request.target);
            if (rules.match == Policy::NetworkRules::Match::denied) {
                access.reason = rules.reason;
                return access;
            }
            if (!decision_allowed() && rules.match == Policy::NetworkRules::Match::allowed) {
                access.action = NetworkAction::allow;
                access.network_decider = hop_decider(request, request_revision, true, {});
                return access;
            }

            Approval approval;
            approval.partially_executed = true;
            approval.call_id = slots[index].call->id;
            approval.tool = slots[index].call->name;
            approval.reason = "web_fetch wants to fetch " + request.url;
            approval.scope = decision_allowed() ? Approval::Scope::web_request : Approval::Scope::network_target;
            if (!decision_allowed())
                approval.session_rule = std::format("Allow connections to {} for this session", key);
            approval.cwd = session_.config().cwd.string();
            approval.mode = policy.planning() ? "plan" : std::string(to_string(policy.mode()));
            if (const auto* tool = std::get_if<std::unique_ptr<PreparedTool>>(&slots[index].action.value()))
                approval.intent = (*tool)->intent();
            approval.requests.push_back({Approval::Request::Kind::network, request.url, "runtime web request"});
            approval.arguments = slots[index].call->arguments;
            identify(approval);
            approval.identity.child_revision = request_revision;
            std::optional<Decision> reviewed;
            if (decision_allowed() && gate_open() && decision_reviewer) {
                ReviewOperation operation;
                operation.identity = approval.identity;
                operation.tool = approval.tool;
                operation.url = request.url;
                operation.target = request.url;
                operation.network = request.target;
                operation.arguments = approval.arguments;
                const ReviewResult result = decision_reviewer->review_action(
                    ReviewScene::fetch_url, operation, review_deadline(), execution_stop);
                approval.review_ids.push_back(result.record_id);
                if (result.disposition == ReviewDisposition::cancelled || execution_stop.stop_requested()) {
                    access.action = NetworkAction::cancel;
                    access.reason = "web request review cancelled";
                    return access;
                }
                if (result.disposition == ReviewDisposition::approve && gate_open()) {
                    approval.authority = ApprovalAuthority::decision_model;
                    reviewed.emplace();
                    reviewed->answer = Decision::Answer::allow;
                    reviewed->state = "approved";
                    reviewed->feedback = result.reason;
                } else {
                    commit_auto_review(approval.call_id, approval.review_ids, "needs_approval", result.reason);
                }
            }
            if (!reviewed && !services_.approver) {
                access.reason = "web request approval is unavailable in this run";
                return access;
            }
            Decision decision = reviewed ? *reviewed : services_.approver(approval, execution_stop);
            validate_decision(approval, decision, execution_stop);
            if (decision.answer == Decision::Answer::allow || decision.answer == Decision::Answer::allow_session) {
                if (!policy.consume_network_approval(request.target, approval, decision)) {
                    decision.answer = Decision::Answer::deny_with_feedback;
                    decision.state = "cancelled";
                    decision.feedback = "Permission changed before the web request was consumed.";
                } else {
                    slots[index].execution->note_authority(approval.authority_stop);
                    ExecutionGrant grant = slots[index].execution->grant;
                    grant.authority = approval.authority;
                    grant.approval = approval.identity;
                    grant.source = decision.answer == Decision::Answer::allow_session ? GrantSource::session
                                                                                     : GrantSource::once;
                    grant.revision = policy.revision();
                    grant.authority_stop = approval.authority_stop;
                    grant.network_targets.push_back(key);
                    committer.commit_permission(approval, decision, &grant);
                    commit_feedback(approval, decision);
                    if (reviewed)
                        commit_auto_review(approval.call_id, approval.review_ids, "approved", decision.feedback);
                    access.action = NetworkAction::allow;
                    access.network_decider = hop_decider(request, grant.revision,
                        decision.answer == Decision::Answer::allow_session, approval.authority_stop);
                    return access;
                }
            }
            committer.commit_permission(approval, decision);
            commit_feedback(approval, decision);
            access.action = NetworkAction::cancel;
            access.reason = decision.feedback.empty() ? "the web request was denied" : decision.feedback;
            if (decision.explicit_denial && decision.state == "denied")
                policy.remember_denied_network(request.target, approval.authority);
            return access;
        };
    };

    const auto make_web_result_scanner = [&](std::size_t index) {
        return [&, index](const WebScanInput& input, std::stop_token execution_stop) -> ScanResult {
            ScanResult scan;
            if (!decision_allowed()) {
                // 未配置决策模型：不扫描，沿用原结果。
                scan.kind = ScanResult::Kind::bypass;
                return scan;
            }
            if (execution_stop.stop_requested() || stop.stop_requested()) {
                scan.kind = ScanResult::Kind::cancelled;
                scan.message = "web content scanning was cancelled";
                return scan;
            }
            if (decision_reviewer) {
                Approval scan_operation;
                scan_operation.call_id = slots[index].call->id;
                identify(scan_operation);
                scan = decision_reviewer->scan_web(input.url, input.result_text, scan_operation.identity,
                                                   review_deadline(), execution_stop);
            } else {
                scan.kind = ScanResult::Kind::unavailable;
                scan.message = "web result withheld: decision review storage is unavailable";
            }
            if (scan.kind == ScanResult::Kind::injection || scan.kind == ScanResult::Kind::unavailable) {
                // 扫描判定完成后立即关闭整轮及子 Agent 的自动审批，再返回工具结果。
                if (services_.review_gate != nullptr)
                    services_.review_gate->disable(scan.kind == ScanResult::Kind::injection
                                                       ? AutoReviewBlock::injection
                                                       : AutoReviewBlock::scan_failed);
                commit_auto_review(slots[index].call->id,
                                   scan.record_id.empty() ? std::vector<std::string>{}
                                                          : std::vector<std::string>{scan.record_id},
                                   scan.kind == ScanResult::Kind::injection ? "needs_approval" : "unavailable",
                                   scan.message);
            }
            return scan;
        };
    };

    const auto execute = [&](Slot& slot, const ExecutionGrant& grant) {
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
    };

    // 串行执行：普通工具写 tool_started 审计；ask/exit_plan 只发实时开始、不写记录。
    const auto run_serial = [&](std::size_t i, ExecutionGrant grant) {
        Slot& slot = slots[i];
        grant.execution_id = execution_id(slot.call->id);
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
            auto decide = make_network_decider(i);
            ExecutionGrant scoped = grant;
            scoped.network_decider = [&](const NetworkTarget& target, std::string& reason, std::stop_token execution_stop) {
                auto promise = std::make_shared<std::promise<std::pair<NetworkAction, std::string>>>();
                auto reply = promise->get_future();
                mailbox.post([&, target, execution_stop, promise] {
                    std::string detail;
                    NetworkAction action = NetworkAction::cancel;
                    try { action = decide(target, detail, execution_stop); }
                    catch (const std::exception& error) { detail = error.what(); }
                    promise->set_value({action, std::move(detail)});
                });
                auto [action, detail] = reply.get();
                reason = std::move(detail);
                return action;
            };
            if (decision_allowed()) {
                const auto url_decider = make_web_request_decider(i);
                const auto scanner = make_web_result_scanner(i);
                scoped.web_request_decider = [&, url_decider](const WebRequestIntent& request, std::stop_token execution_stop) {
                    auto promise = std::make_shared<std::promise<WebRequestAccess>>();
                    auto reply = promise->get_future();
                    mailbox.post([&, request, execution_stop, promise, url_decider] {
                        WebRequestAccess access;
                        access.action = NetworkAction::cancel;
                        try { access = url_decider(request, execution_stop); }
                        catch (const std::exception& error) { access.reason = error.what(); }
                        promise->set_value(std::move(access));
                    });
                    return reply.get();
                };
                scoped.web_result_scanner = [&, scanner](const WebScanInput& input, std::stop_token execution_stop) {
                    auto promise = std::make_shared<std::promise<ScanResult>>();
                    auto reply = promise->get_future();
                    mailbox.post([&, input, execution_stop, promise, scanner] {
                        ScanResult result;
                        try { result = scanner(input, execution_stop); }
                        catch (const std::exception& error) {
                            result.kind = ScanResult::Kind::unavailable;
                            result.message = error.what();
                        }
                        promise->set_value(std::move(result));
                    });
                    return reply.get();
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
    };

    // 并行组：ToolStarted 按顺序在 agent 线程上发；执行分块、每块至多 width 个 jthread，
    // 块内 join 完才起下一块；每个线程只写自己的 slot（docs/design/agent.md §6）。
    const auto run_group = [&] {
        if (group.empty()) return;
        const std::size_t width = group_kind == GroupKind::task
                                      ? static_cast<std::size_t>(
                                            session_.config().options.run.max_parallel_tasks)
                                      : static_cast<std::size_t>(session_.config().options.run.max_parallel_tools);
        for (Pending& p : group) {
            Slot& slot = slots[p.slot];
            p.grant.execution_id = execution_id(slot.call->id);
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

} // namespace dagent::agent
