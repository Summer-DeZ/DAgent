/// @file session.hpp
/// @brief Session：一份对话的长期业务状态（Conversation/WorkPlan/Policy/模型与工具环境/提交器）。
///
/// 只有 SessionCommitter 能修改对话与计划；外部只读取构造请求、快照和策略控制能力。
/// 配置、密钥与终端留在装配层。
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/catalog.hpp"
#include "agent/committer.hpp"
#include "agent/compaction.hpp"
#include "agent/control.hpp"
#include "agent/run.hpp"
#include "agent/events.hpp"
#include "agent/options.hpp"
#include "agent/permission.hpp"
#include "agent/port_journal.hpp"
#include "agent/port_model.hpp"
#include "agent/port_tool.hpp"
#include "agent/public_model.hpp"
#include "agent/run_services.hpp"
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
    SandboxSupport sandbox;        ///< exec::probe() 的中立投影
    PermissionMode permission_mode = PermissionMode::workspace;
    bool read_only = false;
    bool planning = false;
    bool is_main = true; ///< 主会话才在步骤边界处理 MCP 等待/刷新/断连
    int shell_analysis_version = 0; ///< 外围 exec 分析器版本，供权限规则身份使用
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

    /// @brief 提交入口：唯一能修改对话、计划与记录的位置。
    SessionCommitter& committer() { return committer_; }
    /// @brief 只读历史校验（恢复/切模型用）。
    const Conversation& conversation() const { return conversation_; }
    std::optional<std::string> validate() const { return conversation_.validate(); }

    ToolResult activate_skill(std::string_view name);
    Request build_request() const;
    RequestShape request_shape() const;
    ModelParams model_params() const;
    std::size_t estimated_tokens();

    // 运行期协作者：TurnRunner/ActionDispatcher 使用，不向 UI 暴露。
    Policy& policy() { return policy_; }
    const Policy& policy() const { return policy_; }
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
    WorkPlan plan_;
    Conversation conversation_;
    TokenEstimator estimator_;
    Compactor compactor_;
    ControlActionExecutor control_;
    SessionCommitter committer_;
};

} // namespace dagent::agent
