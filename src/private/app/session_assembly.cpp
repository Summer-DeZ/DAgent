#include "app/session_assembly.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

#include "agent/recovery.hpp"
#include "agent/session.hpp"
#include "app/history.hpp"
#include "app/prompt.hpp"
#include "base/log.hpp"
#include "llm/llm.hpp"
#include "exec/shell.hpp"
#include "storage/storage.hpp"

namespace dagent::app {
namespace {

std::shared_ptr<spdlog::logger> log_assembly() { return base::logger("assembly"); }

bool sandbox_available(const exec::Support& support) { return support.read_only_ready(); }

std::string render_prompt(const agent::Setup& setup, const workspace::Environment& env) {
    PromptVars vars;
    vars.model = setup.provider.model;
    vars.project_root = setup.project_root;
    vars.sandbox = sandbox_available(setup.sandbox) &&
                   setup.permission_mode != agent::PermissionMode::unrestricted;
    vars.workspace_sandbox = setup.sandbox.workspace_ready() &&
                             setup.permission_mode != agent::PermissionMode::unrestricted;
    vars.sandbox_backend = setup.sandbox.backend;
    vars.sandbox_missing = setup.sandbox.missing;
    vars.permission_mode = setup.planning ? "plan" : std::string(agent::to_string(setup.permission_mode));
    return render_system_prompt(setup.system_prompt, env, vars);
}

/// @brief 把启动输入的会话级值固化成 SessionConfig；规范化 provider/context 的窗口关系。
agent::SessionConfig make_session_config(agent::Setup& setup, std::string system_prompt) {
    if (setup.provider.context_window == 0) setup.provider.context_window = setup.options.context.window_tokens;
    setup.options.context.window_tokens = setup.provider.context_window;

    agent::SessionConfig config;
    config.options = setup.options;
    config.provider = setup.provider;
    config.system_prompt = std::move(system_prompt);
    config.compact_prompt = setup.compact_prompt;
    config.cwd = setup.cwd;
    config.project_root = setup.project_root;
    config.control_root = setup.control_root;
    config.sandbox_options = agent::SandboxConfig{setup.sandbox_options.version,
                                                  setup.sandbox_options.extra_readable,
                                                  setup.sandbox_options.extra_writable};
    config.sandbox = agent::SandboxSupport{setup.sandbox.backend, setup.sandbox.read_only_ready(),
                                           setup.sandbox.workspace_ready(), setup.sandbox.missing};
    config.permission_mode = setup.permission_mode;
    config.read_only = setup.read_only;
    config.planning = setup.planning;
    config.is_main = setup.subagent_depth == 0;
    config.shell_analysis_version = exec::kShellAnalysisVersion;
    return config;
}

agent::ActionCatalog::Config catalog_config(const agent::Setup& setup) {
    agent::ActionCatalog::Config config;
    config.allowed_tools = setup.allowed_tools;
    config.subagents = setup.subagents;
    config.include_task = setup.subagent_depth == 0 && !setup.subagents.empty();
    return config;
}

/// @brief MCP 步骤边界与断连文本：把具体 MCP 错误翻译成核心的 ResourceError。
class HubResources final : public agent::SessionResources {
public:
    HubResources(std::shared_ptr<Assembly> assembly, tools::Registry& registry)
        : assembly_(std::move(assembly)), registry_(registry) {}

    void begin_step(const agent::Sink& sink, std::stop_token stop) override {
        try {
            assembly_->hub()->apply_pending(registry_, sink, stop);
        } catch (const mcp::McpError& error) {
            throw agent::ResourceError(error.kind() == mcp::McpError::Kind::cancelled
                                            ? agent::ResourceError::Kind::cancelled
                                            : agent::ResourceError::Kind::failed,
                                        error.what());
        }
    }
    std::string mark_disconnected(std::string_view server, std::string_view reason) override {
        return assembly_->hub()->mark_disconnected(server, std::string(reason));
    }
    void report_pending(const agent::Sink& sink) override { assembly_->hub()->report_pending(sink); }

private:
    std::shared_ptr<Assembly> assembly_;
    tools::Registry& registry_;
};

/// @brief 一个已装配会话：owns 工具环境、MCP 资源、记录写入器与核心 Session。
class Instance final : public runtime::SessionInstance {
public:
    Instance(std::shared_ptr<Assembly> assembly, std::shared_ptr<agent::SessionLease> lease,
             std::unique_ptr<tools::Context> context, std::unique_ptr<tools::Registry> registry,
             std::unique_ptr<tools::ToolSession> tools, std::unique_ptr<HubResources> resources,
             std::unique_ptr<agent::Session> session)
        : assembly_(std::move(assembly)), lease_(std::move(lease)), context_(std::move(context)),
          registry_(std::move(registry)), tools_(std::move(tools)), resources_(std::move(resources)),
          session_(std::move(session)) {}

    ~Instance() override {
        if (session_) session_->committer().sync();
    }

    agent::Session& session() override { return *session_; }
    agent::SessionResources& resources() override { return *resources_; }
    std::vector<agent::McpServerState> mcp_states() const override {
        return assembly_->hub()->states();
    }
    std::shared_ptr<agent::SessionLease> lease() const override { return lease_; }

private:
    std::shared_ptr<Assembly> assembly_;
    std::shared_ptr<agent::SessionLease> lease_;
    std::unique_ptr<tools::Context> context_;
    std::unique_ptr<tools::Registry> registry_;
    std::unique_ptr<tools::ToolSession> tools_;
    std::unique_ptr<HubResources> resources_;
    std::unique_ptr<agent::Session> session_;
};

/// @brief 子定义派生：模型外的字段全部按 B18/B19 收窄。
agent::Setup derive_child(const agent::Setup& parent, const agent::SubagentDef& def,
                          const agent::DerivedPermission& permission,
                          const agent::DelegationContext& context) {
    agent::Setup child = parent;
    child.options.run.max_model_calls =
        def.max_model_calls != 0 ? def.max_model_calls : parent.options.run.max_model_calls;
    child.options.run.max_tool_calls =
        def.max_tool_calls != 0 ? def.max_tool_calls : parent.options.run.max_tool_calls;
    child.permission_mode = permission.mode;
    child.read_only = permission.read_only;
    child.planning = permission.planning;
    child.system_prompt = def.system_prompt;
    child.subagent_depth = parent.subagent_depth + 1;
    child.parent_session_id = context.parent_session_id;
    child.subagent_name = def.name;
    child.subagents.clear();
    child.mcp_servers.clear();

    std::vector<std::string> allowed = def.tools.empty() ? context.parent_tools : def.tools;
    std::erase_if(allowed, [&](const std::string& name) {
        if (name == "task" || name == "ask" || name == "exit_plan") return true;
        return def.tools.empty() && name.starts_with("mcp__"); // 默认不含 MCP 工具
    });
    child.allowed_tools = std::move(allowed);
    return child;
}

} // namespace

struct SessionAssembly::Impl {
    Options options;
    std::shared_ptr<Assembly> assembly;

    ModelSelection pick_default() const {
        return ModelSelection{options.base.provider, options.base.model_session};
    }

    ModelSelection pick(const std::string& name) const {
        if (name.empty()) return pick_default();
        if (options.resolve_model) return options.resolve_model(name);
        const auto& models = assembly->models();
        const auto it = models.find(name);
        if (it == models.end()) {
            throw std::runtime_error("unknown model: " + name);
        }
        return ModelSelection{llm::to_public(it->second), assembly->make_model_session(it->second)};
    }

    /// @brief 按 state（可为空 = 启动初值）解析权限与模型，并更新 Setup 的会话级字段。
    agent::Setup prepare_setup(const agent::Setup& base, const std::optional<runtime::SessionState>& state,
                               std::string_view model_name) {
        agent::Setup setup = base;
        if (state) {
            setup.permission_mode = state->mode;
            setup.read_only = state->read_only;
            setup.planning = state->planning;
        }
        const std::string name = !model_name.empty() ? std::string(model_name)
                                                     : (state ? state->model : std::string{});
        const ModelSelection selection = pick(name);
        setup.provider = selection.provider;
        setup.model_session = selection.session;
        return setup;
    }

    std::unique_ptr<Instance> make_instance(agent::Setup setup, agent::SessionConfig config,
                                            agent::SessionMeta meta,
                                            std::unique_ptr<agent::JournalWriter> journal,
                                            std::shared_ptr<agent::SessionLease> lease,
                                            agent::Conversation conversation, agent::WorkPlan plan) {
        const bool child = setup.subagent_depth > 0;
        auto context = std::make_unique<tools::Context>(setup.cwd, setup.tools, setup.files,
                                                        setup.search, setup.process);
        auto registry = std::make_unique<tools::Registry>();
        tools::add_builtin(*registry);
        // 先取 MCP 快照再收窄：定义里没显式写 mcp__* 的子 Agent 默认看不到 MCP 工具。
        if (child && assembly->hub()) assembly->hub()->snapshot(*registry);
        if (!setup.allowed_tools.empty()) registry->retain(setup.allowed_tools);
        auto tools_session = std::make_unique<tools::ToolSession>(*registry, *context);
        auto resources = std::make_unique<HubResources>(assembly, *registry);
        auto session = std::make_unique<agent::Session>(
            std::move(config), std::move(meta), std::move(journal), setup.model_session, *tools_session,
            catalog_config(setup), std::move(conversation), std::move(plan));
        return std::make_unique<Instance>(assembly, std::move(lease), std::move(context),
                                          std::move(registry), std::move(tools_session),
                                          std::move(resources), std::move(session));
    }

    std::unique_ptr<Instance> create_new(const std::optional<runtime::SessionState>& state,
                                         const agent::Sink& replay) {
        agent::Setup setup = prepare_setup(options.base, state, {});
        std::string system_prompt = render_prompt(setup, assembly->environment());
        agent::SessionConfig config = make_session_config(setup, std::move(system_prompt));

        agent::SessionMeta meta;
        meta.id = storage::new_id();
        meta.cwd = setup.cwd;
        meta.git_root = setup.git_root.value_or(std::filesystem::path{});
        meta.model = setup.provider.model;
        auto store = storage::open_store(setup.session);
        auto journal = store->open_writer_create(meta);
        agent::SessionMeta stored = journal->meta();
        auto lease = storage::SessionWriteLease::acquire(setup.session, stored.id);

        auto instance = make_instance(setup, std::move(config), std::move(stored), std::move(journal),
                                      lease, {}, {});
        agent::Session& session = instance->session();
        session.committer().record_system(session.config().system_prompt, session.config().provider.model);
        if (session.committer().broken())
            log_assembly()->error("Failed to write the session record: {}", session.committer().error());
        log_assembly()->info("会话已创建：id={} model={}", session.meta().id, session.meta().model);
        (void)replay;
        return instance;
    }

    std::unique_ptr<Instance> create_child(const agent::DelegationContext& context,
                                           const agent::SubagentDef& def,
                                           const agent::DerivedPermission& permission,
                                           const agent::Sink& replay) {
        agent::Setup setup = derive_child(options.base, def, permission, context);
        const std::string model = !def.model.empty() ? def.model : context.model;
        const ModelSelection selection = pick(model);
        setup.provider = selection.provider;
        setup.model_session = selection.session;

        std::string system_prompt = render_prompt(setup, assembly->environment());
        agent::SessionConfig config = make_session_config(setup, std::move(system_prompt));

        agent::SessionMeta meta;
        meta.id = storage::new_id();
        meta.cwd = setup.cwd;
        meta.git_root = setup.git_root.value_or(std::filesystem::path{});
        meta.model = setup.provider.model;
        meta.parent_id = context.parent_session_id;
        meta.agent_name = def.name;
        auto store = storage::open_store(setup.session);
        auto journal = store->open_writer_create(meta);
        agent::SessionMeta stored = journal->meta();
        auto lease = storage::SessionWriteLease::acquire(setup.session, stored.id);

        auto instance = make_instance(setup, std::move(config), std::move(stored), std::move(journal),
                                      lease, {}, {});
        agent::Session& session = instance->session();
        session.committer().record_system(session.config().system_prompt, session.config().provider.model);
        if (session.committer().broken())
            log_assembly()->error("Failed to write the session record: {}", session.committer().error());
        log_assembly()->info("子会话已创建：id={} agent={} model={}", session.meta().id, meta.agent_name,
                             session.meta().model);
        (void)replay;
        return instance;
    }

    std::unique_ptr<Instance> resume(std::string_view session_id,
                                     const std::optional<runtime::SessionState>& state,
                                     const std::shared_ptr<agent::SessionLease>& reuse_lease,
                                     std::string_view model_name, const agent::Sink& replay) {
        // 显式恢复：先取得可写所有权（切模型复用当前 lease），再做纯恢复读取与校验。
        std::shared_ptr<agent::SessionLease> lease = reuse_lease;
        if (!lease) lease = storage::SessionWriteLease::acquire(options.base.session, session_id);
        auto store = storage::open_store(options.base.session);
        const std::int64_t upper = store->max_seq(session_id);
        std::vector<agent::StoredRecord> records = store->read_records(session_id, 0, upper + 1);
        agent::SessionRecovery recovery;
        agent::RecoveryResult restored = recovery.restore(records);

        agent::Setup setup = prepare_setup(options.base, state, model_name);
        std::string system_prompt = render_prompt(setup, assembly->environment());
        agent::SessionConfig config = make_session_config(setup, std::move(system_prompt));
        auto journal = store->open_writer_resume(session_id);
        agent::SessionMeta meta = journal->meta();
        const std::string previous_model = restored.model.empty() ? meta.model : restored.model;

        auto instance = make_instance(setup, std::move(config), std::move(meta), std::move(journal),
                                      lease, std::move(restored.conversation), std::move(restored.plan));
        agent::Session& session = instance->session();
        if (restored.unfinished) session.committer().repair_crashed_calls(restored.open_calls, replay);

        if (const std::optional<std::string> invalid = session.validate()) {
            throw storage::StorageError(storage::StorageError::Kind::corrupt,
                                        "inconsistent session history: " + *invalid);
        }

        session.committer().record_system(session.config().system_prompt, session.config().provider.model);
        if (previous_model != session.config().provider.model) {
            replay(agent::Notice{agent::Notice::Level::info,
                                 std::format("This session used {}; continuing with {}", previous_model,
                                             session.config().provider.model)});
        }
        if (session.committer().broken()) {
            log_assembly()->error("Failed to write the session record: {}", session.committer().error());
        }
        log_assembly()->info("会话已恢复：id={} model={}", session.meta().id, session.config().provider.model);
        replay(agent::ModelChanged{session.config().provider.model});
        replay(agent::ContextUpdate{{}, session.estimated_tokens(), session.compactor().budget().limit});
        return instance;
    }
};

SessionAssembly::SessionAssembly(Options options)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    impl_->assembly = impl_->options.assembly;
    if (!impl_->assembly) throw std::invalid_argument("session assembly requires an Assembly");
}

SessionAssembly::~SessionAssembly() = default;

std::string SessionAssembly::resolve_session(std::optional<std::string_view> prefix) {
    return app::resolve_session_id(impl_->options.base.session, impl_->options.base.cwd, prefix);
}

const agent::SubagentDef* SessionAssembly::find_subagent(std::string_view name) const {
    return impl_->assembly->find_subagent(name);
}

std::unique_ptr<runtime::SessionInstance> SessionAssembly::create_new(
    std::optional<runtime::SessionState> state, const agent::Sink& replay) {
    return impl_->create_new(state, replay);
}

std::unique_ptr<runtime::SessionInstance> SessionAssembly::resume(std::string_view session_id,
                                                                 std::optional<runtime::SessionState> state,
                                                                 const agent::Sink& replay) {
    return impl_->resume(session_id, state, nullptr, {}, replay);
}

std::unique_ptr<runtime::SessionInstance> SessionAssembly::prepare_switch_model(
    std::string_view model_name, std::shared_ptr<agent::SessionLease> lease,
    const runtime::SessionState& state, const agent::Sink& replay) {
    return impl_->resume(lease->session_id(), state, lease, model_name, replay);
}

std::unique_ptr<runtime::SessionInstance> SessionAssembly::create_child(
    const agent::DelegationContext& context, const agent::SubagentDef& def,
    const agent::DerivedPermission& permission, const agent::Sink& replay) {
    return impl_->create_child(context, def, permission, replay);
}

} // namespace dagent::app
