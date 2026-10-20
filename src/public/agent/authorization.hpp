/// @file authorization.hpp
/// @brief 审批公共步骤、执行前动作授权与单次工具执行期间的授权。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agent/action.hpp"
#include "agent/decision_review.hpp"
#include "agent/events.hpp"

namespace dagent::agent {

class Policy;
class SessionCommitter;
class ActiveExecution;

/// @brief 同批授权共用的依赖和审批身份；引用的会话信息、服务与审阅器由批次保证寿命。
class ApprovalSupport {
public:
    struct SessionInfo {
        const std::string& id;
        const std::filesystem::path& cwd;
        bool is_main;
        std::chrono::system_clock::time_point started;
    };

    ApprovalSupport(Policy&, SessionCommitter&, SessionInfo,
                    const std::shared_ptr<DecisionServices>&, bool decision_ready,
                    std::optional<DecisionReviewer>&, const Approver&,
                    const std::shared_ptr<RunReviewGate>&, std::stop_token);

    const std::string& execution_id(const std::string& call_id);
    void identify(Approval&);
    void validate_decision(const Approval&, Decision&, std::stop_token pending_stop);
    void commit_auto_review(const std::string& call_id, std::vector<std::string> review_ids,
                            std::string_view result, std::string reason);
    void commit_feedback(const Approval&, const Decision&);
    bool decision_allowed();
    bool gate_open();
    ReviewDeadline review_deadline();

private:
    friend class ActionAuthorizer;
    friend class ExecutionAuthorizer;

    Policy& policy;
    SessionCommitter& committer;
    SessionInfo session;
    const std::shared_ptr<DecisionServices>& decisions;
    const bool decision_ready;
    std::optional<DecisionReviewer>& decision_reviewer;
    const Approver& approver;
    const std::shared_ptr<RunReviewGate>& review_gate;
    const std::stop_token stop;
    std::map<std::string, std::string> execution_ids;
    static inline std::atomic<std::uint64_t> sequence{0}; ///< execution/approval 共用的进程级序列。
};

/// @brief 执行前授权；只返回本次结果与停止原因，由批次更新调度状态。
class ActionAuthorizer {
public:
    struct Failure {
        enum class Stop { none, interrupted, denied };
        ToolResult result;
        Stop stop = Stop::none;
    };

    explicit ActionAuthorizer(ApprovalSupport& support) : support_(support) {}
    std::expected<ExecutionGrant, Failure> authorize(const ToolCall&, const PreparedIntent&);

private:
    ApprovalSupport& support_;
};

/// @brief 一次普通工具执行的授权；不持有批次/槽位，不驱动邮箱。
/// 在 executions.begin 后构造，worker join 和邮箱处理结束后、executions.end 前销毁。
class ExecutionAuthorizer {
public:
    ExecutionAuthorizer(ApprovalSupport&, const ToolCall&, const PreparedTool&,
                        std::shared_ptr<ActiveExecution>);

    NetworkAction decide_network(const NetworkTarget&, std::string& reason, std::stop_token);
    WebRequestAccess decide_web_request(const WebRequestIntent&, std::stop_token);
    ScanResult scan_web_result(const WebScanInput&, std::stop_token);

private:
    ApprovalSupport& support_;
    const ToolCall& call_;
    const PreparedTool& tool_;
    std::shared_ptr<ActiveExecution> execution_;
    std::map<std::string, std::pair<std::uint64_t, NetworkAction>> decided_;
};

} // namespace dagent::agent
