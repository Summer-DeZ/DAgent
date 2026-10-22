#include "agent/policy/authorization.hpp"

#include <format>
#include <utility>

#include "agent/record/committer.hpp"
#include "agent/policy/execution_registry.hpp"
#include "agent/policy/permission.hpp"
#include "agent/log.hpp"

namespace dagent::agent {

void ApprovalSupport::cancel_decision(Decision& decision, std::string_view feedback) {
    decision.answer = Decision::Answer::deny_with_feedback;
    decision.state = "cancelled";
    decision.feedback = feedback;
}

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
                ApprovalSupport::cancel_decision(decision, "Permission changed before authorization was consumed; prepare again.");
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
            ApprovalSupport::cancel_decision(decision, "Permission changed before authorization was consumed; prepare again.");
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

} // namespace dagent::agent
