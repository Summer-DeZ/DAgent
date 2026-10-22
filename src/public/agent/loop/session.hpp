/// @file session.hpp
/// @brief Session：一份对话的长期业务状态（Conversation/WorkPlan/Policy/模型与工具环境/提交器）。
///
/// 只有 SessionCommitter 能修改对话与计划；外部只读取构造请求、快照和策略控制能力。
/// 配置、密钥与终端留在装配层。
#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/action.hpp"
#include "agent/decision_types.hpp"
#include "agent/port/decision.hpp"
#include "agent/loop/catalog.hpp"
#include "agent/record/committer.hpp"
#include "agent/record/compaction.hpp"
#include "agent/loop/control.hpp"
#include "agent/policy/execution_registry.hpp"
#include "agent/loop/run.hpp"
#include "agent/events.hpp"
#include "agent/options.hpp"
#include "agent/policy/permission.hpp"
#include "agent/port/journal.hpp"
#include "agent/port/model.hpp"
#include "agent/port/tool.hpp"
#include "agent/public_model.hpp"
#include "agent/loop/run_services.hpp"
#include "agent/session_meta.hpp"
#include "agent/work_plan.hpp"

namespace dagent::agent {

/// @brief 已解析的会话配置：不读配置文件、不含密钥；由装配层提供。
struct SessionConfig {
    std::shared_ptr<const SkillCatalog> skills;
    Options options; ///< 上下文预算与调用上限
    PublicModel provider;
    std::string system_prompt;
    std::string compact_prompt;
    std::filesystem::path cwd, project_root, control_root;
    SandboxConfig sandbox_options; ///< 沙箱持久范围（中立值，映射点只在装配边界）
    SandboxSupport sandbox;        ///< 启动探测的中立投影
    PermissionMode permission_mode = PermissionMode::workspace;
    bool read_only = false;
    bool planning = false;
    /// 本会话用户已确认过 unrestricted 警告；headless 启用不置为 true，之后首次 TUI 进入仍需确认。
    bool unrestricted_acked = false;
    /// 启动即以 unrestricted 时，确认前使用的待确认目标；空表示无（恢复已确认会话也不设置）。
    std::optional<PermissionMode> pending_permission;
    bool is_main = true; ///< 主会话才在步骤边界处理 MCP 等待/刷新/断连
    int shell_analysis_version = 0; ///< 外围 exec 分析器版本，供权限规则身份使用
    /// 决策审阅依赖；为空表示未配置决策模型，保持原有审批行为。
    std::shared_ptr<DecisionServices> decision;
    /// 会话开始时间（恢复保留原会话创建时间）；「本会话修改过」按 mtime 与它比较。
    std::chrono::system_clock::time_point session_started{};
    /// 恢复出的真实用户原话（最近 3 条）；子会话为空。
    std::vector<std::string> recent_user_messages;
    /// 子会话的主要依据（委派任务文本）与委派时父会话的辅助用户原话。
    std::string delegated_task;
    std::vector<std::string> delegated_user_messages;
};

/// @brief 会话的只读快照：UI/协议适配只消费值，不接触内部容器。
struct SessionSnapshot {
    PermissionMode permission_mode = PermissionMode::workspace;
    bool read_only = false;
    bool planning = false;
    TodoView plan;
    std::vector<Policy::SessionGrant> grants;
    std::size_t used_tokens = 0;
    std::size_t token_limit = 0;
};

class Session {
public:
    Session(SessionConfig config, SessionMeta meta, std::unique_ptr<JournalWriter> journal,
            std::shared_ptr<ModelSession> model, ToolSession& tools, ActionCatalog::Config catalog,
            Conversation conversation = {}, WorkPlan plan = {});
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    const SessionMeta& meta() const { return meta_; }
    const SessionConfig& config() const { return config_; }
    bool is_main() const { return config_.is_main; }
    const std::shared_ptr<DecisionServices>& decision() const { return config_.decision; }

    /// @brief 用户是否已确认过本会话的 unrestricted 警告（内存标记或恢复出的 ack 记录）。
    bool unrestricted_acked() const { return unrestricted_acked_; }
    /// @brief 记录用户确认事实：写一条 unrestricted_ack 记录并置内存标记。headless 不调用。
    void mark_unrestricted_acked();

    /// @brief 记录一条真实用户原话（委派任务不计入），只保留最近 3 条；
    /// 独立于压缩后的模型上下文，供决策审阅构造 state。
    void note_user_message(std::string text);
    const std::vector<std::string>& recent_user_messages() const { return recent_user_messages_; }
    /// @brief 构造本轮审阅背景；子会话以委派任务为主要依据。
    ReviewContext review_context() const;

    /// @brief 提交入口：唯一能修改对话、计划与记录的位置。
    SessionCommitter& committer() { return committer_; }
    /// @brief 只读历史校验（恢复/切模型用）。
    const Conversation& conversation() const { return conversation_; }
    std::optional<std::string> validate() const { return conversation_.validate(); }

    ToolResult activate_skill(std::string_view name);
    Request build_request() const;
    RequestShape request_shape() const;
    ModelParams model_params() const;
    /// @brief 在调用线程上构建请求并估算 tokens；同时缓存供跨线程快照读取。
    std::size_t estimated_tokens();

    // 运行期协作者：TurnRunner/ActionDispatcher 使用，不向 UI 暴露。
    Policy& policy() { return policy_; }
    const Policy& policy() const { return policy_; }
    /// @brief 活跃执行登记：调度层登记，runtime 在权限变化时核对并终止。
    ExecutionRegistry& executions() { return executions_; }
    ControlActionExecutor& control() { return control_; }
    ActionCatalog& catalog() { return catalog_; }
    ModelSession& model() { return *model_; }
    Compactor& compactor() { return compactor_; }
    TokenEstimator& estimator() { return estimator_; }

    /// @brief 一轮开始：绑定提交通知出口并重置控制动作的本轮能力。
    void begin_run(const RunServices& services, Run& run);
    void end_run() { committer_.clear_sink(); control_.begin_turn({}); run_ = nullptr; }

    /// @brief 执行线程上的即时快照；由 Runtime 发布给跨线程查询。
    SessionSnapshot snapshot();

private:
    Run* run_ = nullptr; ///< non-owning; bound only during begin_run/end_run
    SessionConfig config_;
    SessionMeta meta_;
    std::unique_ptr<JournalWriter> journal_;
    std::shared_ptr<ModelSession> model_;
    ToolSession& tools_;
    ActionCatalog catalog_;
    Policy policy_;
    ExecutionRegistry executions_;
    WorkPlan plan_;
    Conversation conversation_;
    TokenEstimator estimator_;
    std::atomic<std::size_t> estimated_tokens_{0};
    Compactor compactor_;
    ControlActionExecutor control_;
    SessionCommitter committer_;
    std::vector<std::string> recent_user_messages_; ///< 最近 3 条真实用户原话（最新在末尾）
    bool unrestricted_acked_ = false; ///< 本会话已确认 unrestricted 警告（内存或恢复值）
};

} // namespace dagent::agent
