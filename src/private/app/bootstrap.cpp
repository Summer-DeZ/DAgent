#include "app/bootstrap.hpp"

#include <chrono>
#include <system_error>

#include "exec/sandbox.hpp"
#include "llm/llm.hpp"
#include "workspace/files.hpp"

namespace dagent::app {

std::function<std::shared_ptr<agent::ModelSession>(const llm::ProviderConfig&)>
make_model_factory(const Config& config) {
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

agent::PermissionMode permission_mode(const Config& config, const BootstrapParams& params) {
    // 优先级：--permissions → config.json。
    if (params.permissions) {
        if (*params.permissions == "ask") return agent::PermissionMode::ask;
        if (*params.permissions == "unrestricted") return agent::PermissionMode::unrestricted;
        return agent::PermissionMode::workspace;
    }
    return config.agent.permissions;
}

agent::Setup make_setup(const Config& config, const BootstrapParams& params,
                        const std::shared_ptr<Assembly>& assembly) {
    agent::Setup setup;
    setup.options = config.agent;
    const llm::ProviderConfig& selected = config.models.at(config.model);
    setup.provider = llm::to_public(selected);
    setup.model_session = assembly->make_model_session(selected);

    setup.cwd = params.cwd;
    setup.project_root = config.project_root;
    setup.control_root = config.root;
    std::error_code ec;
    if (std::filesystem::exists(config.project_root / ".git", ec)) setup.git_root = config.project_root;

    setup.tools = config.tools;
    setup.files = config.files;
    setup.search = config.search;
    setup.process = config.process;
    setup.sandbox_options = config.sandbox;
    setup.session = config.session;
    setup.mcp = config.mcp;
    setup.mcp_servers = config.mcp_servers;
    setup.assembly = assembly;
    setup.subagents = config.subagents;

    setup.sandbox = exec::probe();
    setup.permission_mode = permission_mode(config, params);
    setup.read_only = params.read_only;
    setup.planning = params.plan;
    setup.options.read_only = params.read_only;
    setup.system_prompt = workspace::read_text(config.system_prompt_file, config.files).content;
    setup.compact_prompt = workspace::read_text(config.compact_prompt_file, config.files).content;
    return setup;
}

} // namespace dagent::app
