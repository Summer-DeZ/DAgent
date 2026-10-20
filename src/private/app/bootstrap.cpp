#include "app/searxng.hpp"
#include "app/bootstrap.hpp"
#include "app/login.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <system_error>

#include "app/config.hpp"
#include "app/decision_profile.hpp"
#include "app/sandbox_status.hpp"
#include "app/skills.hpp"
#include "app/toolchain.hpp"
#include "app/configuration.hpp"
#include "app/queries.hpp"
#include "app/session_assembly.hpp"
#include "base/log.hpp"
#include "exec/sandbox.hpp"
#include "agent/decision_review.hpp"
#include "llm/decision_provider.hpp"
#include "llm/llm.hpp"
#include "storage/decision_store.hpp"
#include "workspace/context.hpp"
#include "workspace/files.hpp"

namespace dagent::app {
namespace {

using ModelFactory = std::function<std::shared_ptr<agent::ModelSession>(const llm::ProviderConfig&)>;

/// @brief 后端共享的模型客户端工厂：HTTP 调整与重试参数来自配置。
ModelFactory make_model_factory(const Config& config) {
    return [http = config.http, retries = config.agent.run.max_model_retries, paths = HomePaths(config.root)](
               const llm::ProviderConfig& provider) {
        auto configured = provider;
        if (!configured.login.empty()) configured.credentials = {
            paths.credentials / (configured.login + ".json"), paths.credential_locks / (configured.login + ".lock")};
        return llm::make_session(configured, http, agent::RetryOptions{retries});
    };
}

/// @brief 决策审阅依赖：配置了 decision 时创建模型客户端与独立审阅库。
/// 审阅库打不开时停用决策代批；已配置场景仍转人工，网页结果仍须扫描。
std::shared_ptr<agent::DecisionServices> make_decision_services(const Config& config, const HomePaths& paths) {
    if (!config.decision) return nullptr;
    const llm::DecisionProviderConfig& selected = config.decision_models.at(*config.decision);
    const llm::DecisionProviderInfo* info = llm::find_decision_provider(selected.kind);
    if (info == nullptr)
        throw runtime::ConfigurationError("unknown decision provider kind: " + selected.kind);
    auto services = std::make_shared<agent::DecisionServices>();
    services->options = config.decision_review;
    services->version = agent::ReviewVersion{selected.kind,
                                             std::string(info->adapter_version),
                                             selected.endpoint,
                                             selected.model,
                                             selected.model_revision,
                                             std::string(agent::decision_question_version()),
                                             config.decision_review.threshold_profile};
    services->model = llm::make_decision_model(selected, config.decision_review.timeout, config.http);
    try {
        storage::DecisionStoreOptions options;
        options.database = paths.decision_database;
        if (!config.decision_threshold_file.empty())
            options.initial_profile = load_decision_profile(config.decision_threshold_file, services->version);
        services->store = storage::open_decision_store(options);
    } catch (const std::exception& error) {
        base::logger("app")->warn("decision review configuration or storage is unavailable ({}); decision approvals are disabled",
                                  error.what());
        return services; // 保留“已配置”事实，强制复审与网页扫描不能降级为未配置。
    }
    base::logger("app")->info("decision model enabled: {} ({})", selected.name, selected.endpoint);
    return services;
}

agent::PermissionMode permission_mode(const Config& config, const runtime::BootstrapOptions& options) {
    // 优先级：--permissions → config.json。
    if (options.permissions) {
        if (*options.permissions == "ask") return agent::PermissionMode::ask;
        if (*options.permissions == "unrestricted") return agent::PermissionMode::unrestricted;
        return agent::PermissionMode::workspace;
    }
    return config.permissions;
}

/// @brief 会话工厂的装配值：cwd/工具/存储/沙箱探测/提示词与启动权限档。
SessionAssembly::Options session_options(const Config& config, const runtime::BootstrapOptions& options,
                                         const std::shared_ptr<Configuration>& configuration,
                                         const ModelFactory& make_session) {
    SessionAssembly::Options out;
    const HomePaths paths(config.root);
    if (std::filesystem::exists(paths.instructions))
        out.user_instructions = workspace::read_text(paths.instructions, config.files).content;
    out.agent = config.agent;
    out.cwd = options.cwd;
    out.project_root = config.project_root;
    out.control_root = config.root;
    std::error_code ec;
    if (std::filesystem::exists(config.project_root / ".git", ec)) out.git_root = config.project_root;
    out.tools = config.tools;
    if (config.memory.enabled)
        out.memory = std::make_shared<tools::MemoryStore>(paths.memory, paths.project_memory(config.project_root), config.memory);
    auto searxng = std::make_shared<Searxng>(paths, config.searxng_environment, config.tools.web, config.process);
    out.tools.search_endpoint = [searxng](std::stop_token stop) { return searxng->endpoint(stop); };
    out.files = config.files;
    out.search = config.search;
    out.process = config.process;
    out.sandbox_options = config.sandbox;
    // 唯一受限后端是 SRT：真实做一次隔离启动探测；失败时受限命令不启动，只保留显式 host 执行。
    if (auto runtime = sandbox_runtime(paths)) {
        const nlohmann::json probe = sandbox_probe(*runtime, paths.runtime / "sandbox");
        if (probe.value("ok", false)) {
            exec::Support support;
            support.backend = "srt";
            support.child_signals = true;
            out.sandbox = std::move(support);
            out.srt = std::move(runtime);
        } else {
            exec::Support support;
            support.missing.push_back("SRT isolation probe failed: " +
                                      probe.value("error", std::string{"unknown"}));
            out.sandbox = std::move(support);
            base::logger("app")->warn("SRT sandbox is not ready: {}",
                                      probe.value("error", std::string{"unknown"}));
        }
    } else {
        exec::Support support;
        support.missing.push_back("internal/sandbox runtime is not prepared; run `dagent runtime sync`");
        out.sandbox = std::move(support);
    }
    out.tools.sandbox = out.sandbox_options;
    if (out.srt) {
        out.tools.srt = out.srt;
        out.tools.sandbox_state_root = paths.runtime / "sandbox";
    }
    out.search.sandbox.runtime = out.srt;
    out.search.sandbox.state_root = paths.runtime / "sandbox";
    out.search.sandbox.protected_read = {paths.root / "config", paths.root / "data",
                                         paths.root / "logs", paths.root / "run", paths.memory};
    out.storage = config.session;
    out.system_prompt = workspace::read_text(config.system_prompt_file, config.files).content;
    out.compact_prompt = workspace::read_text(config.compact_prompt_file, config.files).content;
    out.initial.mode = permission_mode(config, options);
    out.initial.read_only = options.read_only;
    out.initial.planning = options.plan;
    const llm::ProviderConfig& selected = config.models.at(config.model);
    out.default_model = ModelSelection{llm::to_public(selected), make_session(selected)};
    out.resolve_model = [configuration](const std::string& name) { return configuration->resolve(name); };
    return out;
}

} // namespace

runtime::Assembled assemble_backend(const runtime::BootstrapOptions& options) {
    const HomePaths paths(options.root);
    if (options.mode == "runtime-sync" || options.mode == "runtime-list") {
        Toolchain toolchain(paths);
        runtime::Assembled out;
        out.maintenance = options.mode == "runtime-sync" ? toolchain.sync() : toolchain.status();
        return out;
    }
    if (options.mode == "sandbox-status") {
        runtime::Assembled out;
        out.maintenance = sandbox_status(paths);
        return out;
    }
    if (options.mode == "login") {
        runtime::Assembled out;
        out.login = std::make_shared<Login>(paths);
        return out;
    }
    Config config;
    try {
        config = load_config({options.root, options.cwd, options.overrides});
    } catch (const ConfigError& error) {
        throw runtime::ConfigurationError(error.what());
    }
    if (options.log_level) config.log.level = *options.log_level;
    // 全屏交互界面下写 stderr 会弄花画面；run/查询沿用配置（docs/design/agent.md §12）。
    if (options.mode == "interactive") config.log.also_stderr = false;
    base::init_log(config.log);
    for (const auto& note : config.model_selection_log) base::logger("app")->info("{}", note);

    const ModelFactory make_session = make_model_factory(config);
    auto configuration =
        std::make_shared<Configuration>(options.root, options.overrides, config.models, config.ui.theme_file, config.ui.appearance, make_session);

    auto skills = std::make_shared<agent::SkillCatalog>(*load_skills(paths.skills, config.files));
    for (auto& skill : skills->definitions) {
        const auto name = "skills/" + skill.name;
        if (config.tools.environments.contains(name))
            skill.body += "\n\nFor commands belonging to this skill, set the bash environment parameter to " + name + ".";
    }
    auto memory = std::make_shared<tools::MemoryStore>(paths.memory, paths.project_memory(config.project_root), config.memory);
    runtime::Assembled out;
    out.configuration = configuration;
    out.login = std::make_shared<Login>(paths);

    out.default_model = config.model;
    out.progress_interval_ms = static_cast<int>(config.agent.progress.interval.count());
    if (options.mode == "sessions" || options.mode == "models") {
        out.queries = std::make_shared<QueryGatewayImpl>(config.session, options.cwd, config.project_root,
                                                       config.search, skills, config.process, config.ui.completion_max_files, memory);
        return out;
    }

    workspace::ContextOptions context; context.process = config.process;
    auto session = session_options(config, options, configuration, make_session);
    context.sandbox = session.search.sandbox;
    out.queries = std::make_shared<QueryGatewayImpl>(config.session, options.cwd, config.project_root,
                                                   session.search, skills, config.process, config.ui.completion_max_files, memory);
    // stdio MCP：用受信任 profile + 已解析 SRT 运行时构造受限启动参数；缺 profile 的 server 不启动。
    if (session.srt) {
        for (auto& server : config.mcp_servers) {
            if (server.transport != mcp::Transport::stdio || !server.profile_present) continue;
            mcp::StdioSandbox sandbox;
            sandbox.runtime = *session.srt;
            sandbox.state_root = paths.runtime / "sandbox";
            sandbox.startup_timeout = config.sandbox.startup_timeout;
            exec::Policy policy;
            policy.mode = exec::Mode::workspace_write;
            policy.readable = server.profile_read;
            policy.writable = server.profile_write;
            policy.network_targets = server.profile_network;
            policy.protected_read = {paths.root / "config", paths.root / "data", paths.root / "logs",
                                     paths.root / "run", paths.memory};
            policy.protected_write = policy.protected_read;
            policy.protect_sensitive_names = true;
            policy.private_tmp = true;
            const auto environment = config.tools.environments.find(server.environment);
            if (environment != config.tools.environments.end())
                policy.readable.insert(policy.readable.end(), environment->second.readable.begin(),
                                       environment->second.readable.end());
            if (server.environment == "project") policy.readable.push_back(options.cwd);
            sandbox.policy = std::move(policy);
            server.sandbox = std::move(sandbox);
        }
    }
    session.hub = std::make_shared<tools::McpHub>(config.mcp_servers, config.mcp);
    session.environment = workspace::collect_environment(options.cwd, context);
    session.subagents = std::move(config.subagents);
    session.skills = skills;
    session.decision = make_decision_services(config, paths);
    out.factory = std::make_unique<SessionAssembly>(std::move(session));
    return out;
}

} // namespace dagent::app
