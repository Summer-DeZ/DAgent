#include "agent/authorization.hpp"

#include <format>
#include <utility>

#include "agent/committer.hpp"
#include "agent/execution_registry.hpp"
#include "agent/permission.hpp"
#include "agent/log.hpp"

namespace dagent::agent {
namespace {

void cancel_decision(Decision& decision, std::string_view feedback) {
    decision.answer = Decision::Answer::deny_with_feedback;
    decision.state = "cancelled";
    decision.feedback = feedback;
}

} // namespace

ApprovalSupport::ApprovalSupport(Policy& policy, SessionCommitter& committer, SessionInfo session,
                                const std::shared_ptr<DecisionServices>& decisions, bool decision_ready,
                                std::optional<DecisionReviewer>& decision_reviewer, const Approver& approver,
                                const std::shared_ptr<RunReviewGate>& review_gate, std::stop_token stop)
    : policy(policy), committer(committer), session(session), decisions(decisions),
      decision_ready(decision_ready), decision_reviewer(decision_reviewer), approver(approver),
      review_gate(review_gate), stop(stop) {}

const std::string& ApprovalSupport::execution_id(const std::string& call_id) {
    const auto [execution, inserted] = execution_ids.try_emplace(call_id);
    if (inserted) execution->second = session.id + "/execution/" + std::to_string(++sequence);
    return execution->second;
}

void ApprovalSupport::identify(Approval& approval) {
    approval.identity.request_id = session.id + "/approval/" + std::to_string(++sequence);
    approval.identity.child_session_id = session.id;
    approval.identity.call_id = approval.call_id;
    approval.identity.execution_id = execution_id(approval.call_id);
    const auto live = policy.effective();
    approval.read_only = live.read_only;
    approval.planning = live.planning;
    for (const auto& grant : policy.session_grants()) approval.existing_permissions.push_back(grant.description);
}

void ApprovalSupport::validate_decision(const Approval& approval, Decision& decision, std::stop_token pending_stop) {
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
}

bool ApprovalSupport::gate_open() {
    return review_gate == nullptr || review_gate->enabled();
}

/// 决策模型可用于本会话：只在 auto-edit（workspace、无只读/规划上限）下代批；
/// unrestricted、ask、只读/规划保持原有语义（计划 §2.3/§3）。
/// 子会话 may_ask=false（无人工出口）时也不能借它新增权限。
bool ApprovalSupport::decision_allowed() {
    if (decisions == nullptr) return false;
    if (!session.is_main && !approver) return false;
    const EffectivePermission live = policy.effective();
    return live.mode == PermissionMode::workspace && !live.read_only && !live.planning;
}

ReviewDeadline ApprovalSupport::review_deadline() {
    return std::chrono::steady_clock::now() +
           (decisions != nullptr ? decisions->options.timeout : std::chrono::milliseconds{10000});
}

void ApprovalSupport::commit_auto_review(const std::string& call_id, std::vector<std::string> review_ids,
                                        std::string_view result, std::string reason) {
    AutoReview event;
    event.call_id = call_id;
    event.review_ids = std::move(review_ids);
    event.result = std::string(result);
    event.reason = std::move(reason);
    committer.commit_auto_review(event);
}

/// 人工最终决定回填：只有用户明确批准/拒绝才更新动态阈值；父复批与取消不调整。
void ApprovalSupport::commit_feedback(const Approval& approval, const Decision& decision) {
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
}

std::expected<ExecutionGrant, ActionAuthorizer::Failure>
ActionAuthorizer::authorize(const ToolCall& call, const PreparedIntent& intent) {
    Verdict verdict = support_.policy.evaluate(call, intent);
    if (verdict.kind == Verdict::Kind::deny)
        return std::unexpected(Failure{make_result(std::format(texts::kPolicyDenied, verdict.reason), true, false)});
    if (verdict.kind == Verdict::Kind::allow) return verdict.grant;
    support_.identify(verdict.approval);

    // 决策模型代批：只处理本计划列出的场景（工作区外普通文件读取、MCP 动作）。
    // 不代批的请求直接进入后续审批，不写入会话拒绝规则。
    if (support_.decision_allowed() && support_.gate_open() && support_.decision_reviewer &&
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
                const ReviewResult result = support_.decision_reviewer->review_action(ReviewScene::outside_read, operation,
                                                                    support_.review_deadline(), support_.stop);
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
            const ReviewResult result = support_.decision_reviewer->review_action(ReviewScene::mcp_action, operation,
                                                                support_.review_deadline(), support_.stop);
            review_ids.push_back(result.record_id);
            all_approved = result.disposition == ReviewDisposition::approve;
            review_reason = result.reason;
            review_cancelled = result.disposition == ReviewDisposition::cancelled;
        }
        if (review_cancelled || support_.stop.stop_requested()) {
            support_.commit_auto_review(call.id, review_ids, "cancelled",
                               review_reason.empty() ? "the decision review was cancelled" : review_reason);
            return std::unexpected(Failure{make_result(std::string(texts::kInterruptedCall), false, true), Failure::Stop::interrupted});
        }
        if (all_approved && !support_.gate_open()) {
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
            std::optional<ExecutionGrant> grant = support_.policy.consume_approval(verdict.approval, decision);
            if (!grant) {
                cancel_decision(decision, "Permission changed before authorization was consumed; prepare again.");
                support_.committer.commit_permission(verdict.approval, decision);
                support_.commit_auto_review(call.id, review_ids, "cancelled", decision.feedback);
                return std::unexpected(Failure{make_result(decision.feedback, true, false)});
            }
            grant->execution_id = support_.execution_id(call.id);
            support_.committer.commit_permission(verdict.approval, decision, &*grant);
            support_.commit_auto_review(call.id, review_ids, "approved", decision.feedback);
            if (support_.stop.stop_requested()) {
                return std::unexpected(Failure{make_result(std::string(texts::kInterruptedCall), false, true), Failure::Stop::interrupted});
            }
            return *grant;
        }
        support_.commit_auto_review(call.id, review_ids, "needs_approval",
                           review_reason.empty() ? "the decision model did not approve this request"
                                                 : review_reason);
        // 切换到后续审批时保留前序审阅编号（计划 §8.1）。
        verdict.approval.review_ids = review_ids;
    }

    if (!support_.approver)
        return std::unexpected(Failure{make_result(std::format(texts::kApprovalUnavailable, verdict.approval.reason), true, false)});
    auto decision = support_.approver(verdict.approval, support_.stop);
    support_.validate_decision(verdict.approval, decision, support_.stop);
    std::optional<ExecutionGrant> grant;
    if (decision.answer == Decision::Answer::allow || decision.answer == Decision::Answer::allow_session) {
        grant = support_.policy.consume_approval(verdict.approval, decision);
        if (grant) grant->execution_id = support_.execution_id(call.id);
        else {
            cancel_decision(decision, "Permission changed before authorization was consumed; prepare again.");
        }
    }
    support_.committer.commit_permission(verdict.approval, decision, grant ? &*grant : nullptr);
    support_.commit_feedback(verdict.approval, decision);
    if (support_.stop.stop_requested()) {
        return std::unexpected(Failure{make_result(std::string(texts::kInterruptedCall), false, true), Failure::Stop::interrupted});
    }
    if (decision.answer == Decision::Answer::deny) {
        return std::unexpected(Failure{make_result(std::string(texts::kDenied), true, false), Failure::Stop::denied});
    }
    if (!grant) {
        const std::string feedback = verdict.approval.authority == ApprovalAuthority::parent_model
            ? "Parent permission review " + decision.state + ": " + decision.feedback
            : "Permission request denied: " + decision.feedback;
        return std::unexpected(Failure{make_result(feedback, true, false)});
    }
    return *grant;
}

ExecutionAuthorizer::ExecutionAuthorizer(ApprovalSupport& support, const ToolCall& call,
                                        const PreparedTool& tool, std::shared_ptr<ActiveExecution> execution)
    : support_(support), call_(call), tool_(tool), execution_(std::move(execution)) {}

void ExecutionAuthorizer::complete_runtime_approval(Approval& approval, std::uint64_t request_revision) {
    approval.partially_executed = true;
    approval.call_id = call_.id;
    approval.tool = call_.name;
    approval.cwd = support_.session.cwd.string();
    approval.mode = support_.policy.planning() ? "plan" : std::string(to_string(support_.policy.mode()));
    approval.arguments = call_.arguments;
    support_.identify(approval);
    approval.identity.child_revision = request_revision;
}

ExecutionGrant ExecutionAuthorizer::commit_network_grant(const Approval& approval, const Decision& decision,
                                                        GrantSource source, const std::string& target) {
    ExecutionGrant grant = execution_->grant;
    grant.authority = approval.authority;
    grant.approval = approval.identity;
    grant.source = source;
    grant.revision = support_.policy.revision();
    grant.authority_stop = approval.authority_stop;
    grant.network_targets.push_back(target);
    support_.committer.commit_permission(approval, decision, &grant);
    return grant;
}

// 运行中网络判定：先查配置/会话拒绝；需要时做脚本强制复审与 d3 代批；再走现有审批器。
// Dispatcher consumes requests from the execution mailbox; authorization runs on that thread.
NetworkAction ExecutionAuthorizer::decide_network(const NetworkTarget& target, std::string& reason,
                                                 std::stop_token execution_stop) {
    if (execution_stop.stop_requested() || support_.stop.stop_requested()) {
        reason = "execution cancelled before network authorization";
        return NetworkAction::cancel;
    }
    const std::string key = target.host + ":" + std::to_string(target.port);
    const auto cancel = [&] {
        if (!support_.stop.stop_requested() && execution_ != nullptr)
            execution_->note_termination(reason);
        return NetworkAction::cancel;
    };
    const auto request_revision = support_.policy.revision();
    if (const auto found = decided_.find(key); found != decided_.end() && found->second.first == request_revision)
        return found->second.second;
    const auto remember = [&](NetworkAction action, bool persisted = false) {
        decided_.insert_or_assign(key, std::pair{request_revision, action});
        // persisted 表示批准来自配置/会话规则，撤销这类规则时要能终止使用它的执行。
        if (action == NetworkAction::allow && execution_ != nullptr)
            execution_->note_network(target, persisted);
        if (action == NetworkAction::cancel) return cancel();
        return action;
    };

    const Policy::NetworkRules rules = support_.policy.inspect_network(target);
    if (support_.policy.revision() != request_revision) {
        reason = "permission changed during network authorization";
        return cancel();
    }
    if (rules.match == Policy::NetworkRules::Match::denied) {
        reason = rules.reason;
        return remember(NetworkAction::deny);
    }

    const PreparedIntent& intent = tool_.intent();
    const CommandIntent* command = intent.command ? &*intent.command : nullptr;
    const bool has_targets = command != nullptr && !command->script_targets.empty();
    // 本轮自动审批关闭：跳过 d3 与父复批，直接交给人工（计划 2.1/2.9）。
    const bool review_enabled = support_.decision_allowed() && support_.gate_open() && support_.decision_reviewer.has_value();
    std::vector<ScriptEvidence> evidence;
    bool must_review = false;
    if (support_.decision_allowed() && has_targets) {
        for (const auto& script : command->script_targets) {
            const auto relative = script.path.lexically_relative(support_.session.cwd);
            PreparedIntent reading;
            reading.kind = ToolKind::read;
            reading.paths.push_back({script.path, Access::read,
                !relative.empty() && !relative.is_absolute() && *relative.begin() != ".."});
            if (support_.policy.evaluate(call_, reading).kind != Verdict::Kind::allow) {
                ScriptEvidence missing;
                missing.target = script.path;
                missing.kind = ScriptEvidence::Kind::unavailable;
                missing.reason = "script evidence requires file read approval";
                evidence.push_back(std::move(missing));
                must_review = true;
                continue;
            }
            auto item = tool_.script_evidence(script, support_.session.started,
                                                 support_.review_deadline(), execution_stop);
            if (execution_stop.stop_requested() || support_.stop.stop_requested()) return cancel();
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
    approval.reason = std::format("{} wants to connect to {}", call_.name, key);
    approval.intent = intent;
    approval.requests.push_back({Approval::Request::Kind::network, key, "runtime network request"});
    complete_runtime_approval(approval, request_revision);
    bool approved_by_review = false;
    std::string review_reason;
    std::vector<std::string> review_ids;
    bool scripts_passed = true;

    if (review_enabled && has_targets) {
        // 本会话修改过的目标必须复审，不能因 host 已放行而跳过（计划 2.2/2.4）。
        const ReviewOperation operation = [&] {
            ReviewOperation value;
            value.identity = approval.identity;
            value.tool = call_.name;
            value.command = command->command;
            value.arguments = call_.arguments;
            value.network = target;
            value.file_execution = true;
            value.mixed_scripts = command->script_targets.size() > 1;
            return value;
        }();
        for (std::size_t i = 0; i < command->script_targets.size(); ++i) {
            const ScriptTarget& script = command->script_targets[i];
            const ReviewOperation scoped = operation;
            const ReviewResult result = support_.decision_reviewer->review_script(scoped, script, evidence[i],
                                                                support_.review_deadline(), execution_stop);
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
        operation.tool = call_.name;
        operation.command = command != nullptr ? command->command : std::string{};
        operation.arguments = call_.arguments;
        operation.network = target;
        operation.file_execution = command != nullptr && command->has_unresolved_execution;
        const ReviewResult result = support_.decision_reviewer->review_action(ReviewScene::command_network, operation,
                                                            support_.review_deadline(), execution_stop);
        review_ids.push_back(result.record_id);
        if (result.disposition == ReviewDisposition::cancelled) {
            reason = result.reason;
            return cancel();
        }
        if (result.disposition == ReviewDisposition::approve) approved_by_review = true;
        else if (review_reason.empty()) review_reason = result.reason;
    }

    if (approved_by_review) {
        if (!support_.gate_open()) {
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
            if (!support_.policy.consume_network_approval(target, approval, decision)) {
                cancel_decision(decision, "Permission changed before network authorization was consumed.");
                support_.committer.commit_permission(approval, decision);
                reason = decision.feedback;
                return remember(NetworkAction::cancel);
            }
            execution_->note_authority(approval.authority_stop);
            ExecutionGrant network_grant = commit_network_grant(approval, decision, GrantSource::once, key);
            support_.commit_auto_review(call_.id, review_ids, "approved", decision.feedback);
            if (execution_stop.stop_requested() || approval.authority_stop.stop_requested()) {
                reason = "permission revoked before network approval was applied";
                return cancel();
            }
            return remember(NetworkAction::allow);
        }
    } else if (review_enabled && !review_ids.empty()) {
        // d3 未批准：不写入会话拒绝规则，转后续审批。
        support_.commit_auto_review(call_.id, review_ids, "needs_approval",
                           review_reason.empty() ? "the decision model did not approve this request"
                                                 : review_reason);
    }

    if (!support_.approver) {
        reason = review_reason.empty() ? "runtime network approval is unavailable in this run" : review_reason;
        return remember(NetworkAction::deny);
    }

    approval.authority = ApprovalAuthority::user;
    approval.session_rule = std::format("Allow connections to {} for this session", key);
    approval.review_ids = review_ids;
    Decision decision = support_.approver(approval, execution_stop);
    support_.validate_decision(approval, decision, execution_stop);
    if (decision.answer == Decision::Answer::allow || decision.answer == Decision::Answer::allow_session) {
        if (!support_.policy.consume_network_approval(target, approval, decision)) {
            cancel_decision(decision, "Permission changed before network authorization was consumed.");
            support_.committer.commit_permission(approval, decision);
            reason = decision.feedback;
            return remember(NetworkAction::cancel);
        }
        execution_->note_authority(approval.authority_stop);
        ExecutionGrant network_grant = commit_network_grant(approval, decision,
            decision.answer == Decision::Answer::allow_session ? GrantSource::session : GrantSource::once, key);
        support_.commit_feedback(approval, decision);
        if (execution_stop.stop_requested() || approval.authority_stop.stop_requested()) {
            reason = "permission revoked before network approval was applied";
            return cancel();
        }
        return remember(NetworkAction::allow, decision.answer == Decision::Answer::allow_session);
    }
    support_.committer.commit_permission(approval, decision);
    support_.commit_feedback(approval, decision);
    reason = decision.answer == Decision::Answer::deny_with_feedback ? decision.feedback
        : approval.authority == ApprovalAuthority::parent_model ? "the parent agent denied network access"
                                                                 : "the user denied network access";
    if (decision.explicit_denial && decision.state == "denied")
        support_.policy.remember_denied_network(target, approval.authority);
    return remember(NetworkAction::cancel);
}

// web_fetch 逐跳 URL 审阅与结果扫描：审阅与扫描都走 execution mailbox（在执行线程上编排）。
WebRequestAccess ExecutionAuthorizer::decide_web_request(const WebRequestIntent& request,
                                                       std::stop_token execution_stop) {
    WebRequestAccess access;
    access.action = NetworkAction::deny;
    if (execution_stop.stop_requested() || support_.stop.stop_requested()) {
        access.action = NetworkAction::cancel;
        access.reason = "execution cancelled before web request authorization";
        return access;
    }
    const std::string key = request.target.host + ":" + std::to_string(request.target.port);
    const auto request_revision = support_.policy.revision();
    const auto hop_decider = [this](WebRequestIntent reviewed, std::uint64_t revision,
                                   bool persisted, std::stop_token authority_stop) {
        return [this, reviewed, revision, persisted, authority_stop](const NetworkTarget& target,
            std::string& reason, std::stop_token token) -> NetworkAction {
            if (token.stop_requested() || support_.stop.stop_requested() || authority_stop.stop_requested() ||
                support_.policy.revision() != revision) {
                reason = "permission changed or execution cancelled before the web request was sent";
                return NetworkAction::cancel;
            }
            if (target.host != reviewed.target.host || target.port != reviewed.target.port) {
                reason = "the connection target does not match the reviewed URL";
                return NetworkAction::cancel;
            }
            if (support_.policy.inspect_network(target).match == Policy::NetworkRules::Match::denied) {
                reason = "network target is denied";
                return NetworkAction::deny;
            }
            execution_->note_network(target, persisted);
            return NetworkAction::allow;
        };
    };
    const Policy::NetworkRules rules = support_.policy.inspect_network(request.target);
    if (rules.match == Policy::NetworkRules::Match::denied) {
        access.reason = rules.reason;
        return access;
    }
    if (!support_.decision_allowed() && rules.match == Policy::NetworkRules::Match::allowed) {
        access.action = NetworkAction::allow;
        access.network_decider = hop_decider(request, request_revision, true, {});
        return access;
    }

    Approval approval;
    approval.reason = "web_fetch wants to fetch " + request.url;
    approval.scope = support_.decision_allowed() ? Approval::Scope::web_request : Approval::Scope::network_target;
    if (!support_.decision_allowed())
        approval.session_rule = std::format("Allow connections to {} for this session", key);
    approval.intent = tool_.intent();
    approval.requests.push_back({Approval::Request::Kind::network, request.url, "runtime web request"});
    complete_runtime_approval(approval, request_revision);
    std::optional<Decision> reviewed;
    if (support_.decision_allowed() && support_.gate_open() && support_.decision_reviewer) {
        ReviewOperation operation;
        operation.identity = approval.identity;
        operation.tool = approval.tool;
        operation.url = request.url;
        operation.target = request.url;
        operation.network = request.target;
        operation.arguments = approval.arguments;
        const ReviewResult result = support_.decision_reviewer->review_action(
            ReviewScene::fetch_url, operation, support_.review_deadline(), execution_stop);
        approval.review_ids.push_back(result.record_id);
        if (result.disposition == ReviewDisposition::cancelled || execution_stop.stop_requested()) {
            access.action = NetworkAction::cancel;
            access.reason = "web request review cancelled";
            return access;
        }
        if (result.disposition == ReviewDisposition::approve && support_.gate_open()) {
            approval.authority = ApprovalAuthority::decision_model;
            reviewed.emplace();
            reviewed->answer = Decision::Answer::allow;
            reviewed->state = "approved";
            reviewed->feedback = result.reason;
        } else {
            support_.commit_auto_review(approval.call_id, approval.review_ids, "needs_approval", result.reason);
        }
    }
    if (!reviewed && !support_.approver) {
        access.reason = "web request approval is unavailable in this run";
        return access;
    }
    Decision decision = reviewed ? *reviewed : support_.approver(approval, execution_stop);
    support_.validate_decision(approval, decision, execution_stop);
    if (decision.answer == Decision::Answer::allow || decision.answer == Decision::Answer::allow_session) {
        if (!support_.policy.consume_network_approval(request.target, approval, decision)) {
            cancel_decision(decision, "Permission changed before the web request was consumed.");
        } else {
            execution_->note_authority(approval.authority_stop);
            ExecutionGrant grant = commit_network_grant(approval, decision,
                decision.answer == Decision::Answer::allow_session ? GrantSource::session : GrantSource::once, key);
            support_.commit_feedback(approval, decision);
            if (reviewed)
                support_.commit_auto_review(approval.call_id, approval.review_ids, "approved", decision.feedback);
            access.action = NetworkAction::allow;
            access.network_decider = hop_decider(request, grant.revision,
                decision.answer == Decision::Answer::allow_session, approval.authority_stop);
            return access;
        }
    }
    support_.committer.commit_permission(approval, decision);
    support_.commit_feedback(approval, decision);
    access.action = NetworkAction::cancel;
    access.reason = decision.feedback.empty() ? "the web request was denied" : decision.feedback;
    if (decision.explicit_denial && decision.state == "denied")
        support_.policy.remember_denied_network(request.target, approval.authority);
    return access;
}

ScanResult ExecutionAuthorizer::scan_web_result(const WebScanInput& input, std::stop_token execution_stop) {
    ScanResult scan;
    if (!support_.decision_allowed()) {
        // 未配置决策模型：不扫描，沿用原结果。
        scan.kind = ScanResult::Kind::bypass;
        return scan;
    }
    if (execution_stop.stop_requested() || support_.stop.stop_requested()) {
        scan.kind = ScanResult::Kind::cancelled;
        scan.message = "web content scanning was cancelled";
        return scan;
    }
    if (support_.decision_reviewer) {
        Approval scan_operation;
        scan_operation.call_id = call_.id;
        support_.identify(scan_operation);
        scan = support_.decision_reviewer->scan_web(input.url, input.result_text, scan_operation.identity,
                                           support_.review_deadline(), execution_stop);
    } else {
        scan.kind = ScanResult::Kind::unavailable;
        scan.message = "web result withheld: decision review storage is unavailable";
    }
    if (scan.kind == ScanResult::Kind::injection || scan.kind == ScanResult::Kind::unavailable) {
        // 扫描判定完成后立即关闭整轮及子 Agent 的自动审批，再返回工具结果。
        if (support_.review_gate != nullptr)
            support_.review_gate->disable(scan.kind == ScanResult::Kind::injection
                                               ? AutoReviewBlock::injection
                                               : AutoReviewBlock::scan_failed);
        support_.commit_auto_review(call_.id,
                           scan.record_id.empty() ? std::vector<std::string>{}
                                                  : std::vector<std::string>{scan.record_id},
                           scan.kind == ScanResult::Kind::injection ? "needs_approval" : "unavailable",
                           scan.message);
    }
    return scan;
}

} // namespace dagent::agent
