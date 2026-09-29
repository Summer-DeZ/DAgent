#include "app/bootstrap.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <system_error>

#include "app/config.hpp"
#include "app/sandbox_status.hpp"
#include "app/skills.hpp"
#include "app/toolchain.hpp"
#include "app/configuration.hpp"
#include "app/queries.hpp"
#include "app/session_assembly.hpp"
#include "base/log.hpp"
#include "exec/sandbox.hpp"
#include "llm/llm.hpp"
#include "workspace/context.hpp"
#include "workspace/files.hpp"

namespace dagent::app {
namespace {

using ModelFactory = std::function<std::shared_ptr<agent::ModelSession>(const llm::ProviderConfig&)>;

/// @brief 后端共享的模型客户端工厂：HTTP 调整与重试参数来自配置。
ModelFactory make_model_factory(const Config& config) {
    return [http = config.http, retries = config.agent.run.max_model_retries](
               const llm::ProviderConfig& provider) {
        return llm::make_session(provider, http, agent::RetryOptions{retries});
    };
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
    out.files = config.files;
    out.search = config.search;
    out.process = config.process;
    out.sandbox_options = config.sandbox;
    // SRT 是首选后端：真实做一次隔离启动探测；不可用时退回现有 Landlock 后端（S09 再移除）。
    if (auto runtime = sandbox_runtime(paths)) {
        const nlohmann::json probe = sandbox_probe(*runtime, paths.runtime / "sandbox");
        if (probe.value("ok", false)) {
            exec::Support support;
            support.backend = "srt";
            support.filesystem_write = true;
            support.filesystem_read = true;
            support.protected_subpaths = true;
            support.private_tmp = true;
            support.network_block = true;
            support.local_socket_block = true;
            support.process_control_block = true;
            support.child_signals = true;
            out.sandbox = std::move(support);
            out.srt = std::move(runtime);
        } else {
            out.sandbox = exec::probe();
            base::logger("app")->warn("SRT sandbox is not ready ({}); falling back to the Landlock backend",
                                      probe.value("error", std::string{"unknown"}));
        }
    } else {
        out.sandbox = exec::probe();
    }
    if (out.srt) {
        out.tools.srt = out.srt;
        out.tools.sandbox_state_root = paths.runtime / "sandbox";
    }
    out.search.sandbox.runtime = out.srt;
    out.search.sandbox.state_root = paths.runtime / "sandbox";
    out.search.sandbox.protected_read = {paths.root / "config", paths.root / "data",
                                         paths.root / "logs", paths.root / "run"};
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
        std::make_shared<Configuration>(options.root, options.overrides, config.models, config.ui.theme_file, make_session);

    auto skills = std::make_shared<agent::SkillCatalog>(*load_skills(paths.skills, config.files));
    for (auto& skill : skills->definitions) {
        const auto name = "skills/" + skill.name;
        if (config.tools.environments.contains(name))
            skill.body += "\n\nFor commands belonging to this skill, set the bash environment parameter to " + name + ".";
    }
    runtime::Assembled out;
    out.configuration = configuration;

    out.default_model = config.model;
    out.progress_interval_ms = static_cast<int>(config.agent.progress.interval.count());
    if (options.mode == "sessions" || options.mode == "models") {
        out.queries = std::make_shared<QueryGatewayImpl>(config.session, options.cwd, config.project_root,
                                                       config.search, skills, config.process, config.ui.completion_max_files);
        return out;
    }

    workspace::ContextOptions context; context.process = config.process;
    auto session = session_options(config, options, configuration, make_session);
    context.sandbox = session.search.sandbox;
    out.queries = std::make_shared<QueryGatewayImpl>(config.session, options.cwd, config.project_root,
                                                   session.search, skills, config.process, config.ui.completion_max_files);
    session.hub = std::make_shared<tools::McpHub>(config.mcp_servers, config.mcp);
    session.environment = workspace::collect_environment(options.cwd, context);
    session.subagents = std::move(config.subagents);
    session.skills = skills;
    out.factory = std::make_unique<SessionAssembly>(std::move(session));
    return out;
}

} // namespace dagent::app
