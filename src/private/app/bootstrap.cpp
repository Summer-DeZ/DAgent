#include "app/bootstrap.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <system_error>

#include "app/assembly.hpp"
#include "app/config.hpp"
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
        net::HttpOptions adjusted = http;
        adjusted.timeout = std::chrono::seconds{0};
        if (adjusted.idle_timeout == std::chrono::seconds{0}) {
            adjusted.idle_timeout = std::chrono::seconds{120};
        }
        return llm::make_session(provider, adjusted, agent::RetryOptions{retries});
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
                                         const std::shared_ptr<Assembly>& assembly,
                                         const std::shared_ptr<Configuration>& configuration) {
    SessionAssembly::Options out;
    out.assembly = assembly;
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
    out.sandbox = exec::probe();
    out.storage = config.session;
    out.system_prompt = workspace::read_text(config.system_prompt_file, config.files).content;
    out.compact_prompt = workspace::read_text(config.compact_prompt_file, config.files).content;
    out.initial.mode = permission_mode(config, options);
    out.initial.read_only = options.read_only;
    out.initial.planning = options.plan;
    const llm::ProviderConfig& selected = config.models.at(config.model);
    out.default_model = ModelSelection{llm::to_public(selected), assembly->make_model_session(selected)};
    out.resolve_model = [configuration](const std::string& name) { return configuration->resolve(name); };
    return out;
}

} // namespace

runtime::Assembled assemble_backend(const runtime::BootstrapOptions& options) {
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
        std::make_shared<Configuration>(options.root, options.cwd, options.overrides, config, make_session);

    runtime::Assembled out;
    out.configuration = configuration;
    out.queries = std::make_shared<QueryGatewayImpl>(config.session, options.cwd, config.project_root,
                                                     config.search);
    out.default_model = config.model;
    out.progress_interval_ms = static_cast<int>(config.agent.progress.interval.count());
    if (options.mode == "sessions" || options.mode == "models") return out;

    auto assembly = Assembly::create(config.mcp_servers, config.mcp,
                                     workspace::collect_environment(options.cwd, {}), config.subagents,
                                     config.models, make_session);
    out.factory = std::make_unique<SessionAssembly>(session_options(config, options, assembly, configuration));
    return out;
}

} // namespace dagent::app
