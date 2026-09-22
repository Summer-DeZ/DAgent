#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include <signal.h>

#include "agent/headless.hpp"
#include "agent/setup.hpp"
#include "app/assembly.hpp"
#include "app/cli.hpp"
#include "app/configuration.hpp"
#include "app/history.hpp"
#include "app/launcher.hpp"
#include "app/queries.hpp"
#include "app/session_assembly.hpp"
#include "base/log.hpp"
#include "exec/sandbox.hpp"
#include "llm/llm.hpp"
#include "runtime/runtime.hpp"
#include "tui/grapheme.hpp"
#include "protocol/dto.hpp"
#include "protocol/rpc.hpp"
#include "ui/shell.hpp"
#include "workspace/files.hpp"

namespace {

namespace fs = std::filesystem;
using dagent::app::Mode;
using dagent::llm::ProviderConfig;

// docs/design/agent.md §12：整个进程忽略 SIGPIPE，写关闭的管道得到 EPIPE 而不是被信号杀死。
// exec 层只会在 SIGPIPE 仍是默认处理时设置它，两者不冲突。
void ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); }

dagent::agent::PermissionMode
permission_mode(const dagent::app::Config& config, const dagent::app::Args& args) {
    // 优先级：--permissions → config.json。
    if (args.permissions) {
        if (*args.permissions == "ask") return dagent::agent::PermissionMode::ask;
        if (*args.permissions == "unrestricted") return dagent::agent::PermissionMode::unrestricted;
        return dagent::agent::PermissionMode::workspace;
    }
    return config.agent.permissions;
}

dagent::agent::Setup make_setup(const dagent::app::Config& config, const dagent::app::Args& args,
                                const std::shared_ptr<dagent::app::Assembly>& assembly) {
    namespace agent = dagent::agent;

    agent::Setup setup;
    setup.options = config.agent;
    const ProviderConfig& selected = config.models.at(config.model);
    setup.provider = dagent::llm::to_public(selected);
    setup.model_session = assembly->make_model_session(selected);

    setup.cwd = args.cwd;
    setup.project_root = config.project_root;
    setup.control_root = config.root;
    std::error_code ec;
    if (fs::exists(config.project_root / ".git", ec)) setup.git_root = config.project_root;

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

    setup.sandbox = dagent::exec::probe();
    setup.permission_mode = permission_mode(config, args);
    setup.read_only = args.read_only;
    setup.planning = args.plan;
    setup.options.read_only = args.read_only;
    setup.system_prompt =
        dagent::workspace::read_text(config.system_prompt_file, config.files).content;
    setup.compact_prompt =
        dagent::workspace::read_text(config.compact_prompt_file, config.files).content;
    return setup;
}

std::pair<std::string, int> fit_title(std::string_view title, int columns) {
    std::string out;
    int width = 0;
    dagent::tui::unicode::Grapheme grapheme;
    while (dagent::tui::unicode::next_grapheme(title, grapheme)) {
        if (width + grapheme.width > columns) break;
        out.append(grapheme.bytes);
        width += grapheme.width;
    }
    return {std::move(out), width};
}

std::string local_minute(std::chrono::system_clock::time_point time) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    if (::localtime_r(&raw, &local) == nullptr) return "0000-00-00 00:00";
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M");
    return out.str();
}

time_t local_from_ms(std::int64_t milliseconds) {
    return static_cast<std::time_t>(milliseconds / 1000);
}

void print_sessions(const nlohmann::json& sessions) {
    if (sessions.empty() || !sessions.is_array()) {
        std::cout << "no sessions for this working directory\n";
        return;
    }
    for (const auto& session : sessions) {
        const std::string title_value = session.value("title", "");
        auto [title, width] = fit_title(title_value, 50);
        const std::int64_t updated = session.value("updated", std::int64_t{0});
        std::cout << local_minute(std::chrono::system_clock::from_time_t(local_from_ms(updated)))
                  << "   " << title << std::string(static_cast<std::size_t>(50 - width), ' ')
                  << "   " << session.value("id", "") << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    // 必须在创建任何线程之前（spdlog、stdin 读取都会建线程）：之后创建的线程都继承这个屏蔽。
    dagent::agent::Interrupts& interrupts = dagent::agent::install_interrupts();
    ignore_sigpipe();

    const std::variant<dagent::app::Args, int> parsed = dagent::app::parse_args(argc, argv);
    if (const int* code = std::get_if<int>(&parsed)) return *code;
    const dagent::app::Args& args = std::get<dagent::app::Args>(parsed);

    try {
        const dagent::app::InstallationPaths paths = dagent::app::installation_paths();

        // 查询模式：启动本前端独占的后端，经私有协议完成初始化与查询（R09）。
        // 启动配置与密钥只在后端解析；前端不读配置文件。
        if (args.mode == Mode::models || args.mode == Mode::sessions) {
            dagent::app::BackendLaunch launch;
            launch.root = paths.root;
            launch.cwd = args.cwd;
            launch.overrides = args.overrides;
            launch.mode = args.mode == Mode::models ? "models" : "sessions";
            launch.log_level = args.log_level;
            dagent::app::BackendSession session =
                dagent::app::BackendSession::start(launch, dagent::client::Client::Callbacks{});
            if (args.mode == Mode::models) {
                const nlohmann::json models = session.client().call("model.list");
                const std::string default_name = models.value("default_name", "");
                std::cout << "NAME\tKIND\tMODEL\tBASE_URL\tKEY\n";
                for (const auto& model : models["models"]) {
                    const std::string name = model.value("name", "");
                    std::cout << name << (name == default_name ? " *" : "") << '\t'
                              << model.value("kind", "") << '\t' << model.value("model", "") << '\t'
                              << model.value("base_url", "") << '\t'
                              << (model.value("has_key", false) ? "yes" : "no") << '\n';
                }
            } else {
                const nlohmann::json sessions = session.client().call("session.list", {{"limit", 20}});
                print_sessions(sessions["sessions"]);
            }
            return 0;
        }

        dagent::app::Config config = dagent::app::load_config({paths.root, args.cwd, args.overrides});
        if (args.log_level) config.log.level = *args.log_level;
        // 全屏界面下写 stderr 会弄花画面；run 模式按 log.also_stderr 配置。
        if (args.mode == Mode::interactive) config.log.also_stderr = false;
        dagent::base::init_log(config.log);
        for (const auto& note : config.model_selection_log) dagent::base::logger("app")->info("{}", note);
        if (!config.subagents.empty()) {
            std::string names;
            for (const auto& def : config.subagents) {
                if (!names.empty()) names += "、";
                names += def.name;
            }
            dagent::base::logger("app")->info("loaded {} subagent definitions: {}", config.subagents.size(), names);
        }

        // 模型客户端工厂持有 HTTP 调整与重试参数；密钥只经过 llm 内部配置，不进入公开描述。
        auto make_session = [http = config.http,
                             retries = config.agent.run.max_model_retries](const ProviderConfig& provider) {
            dagent::net::HttpOptions adjusted = http;
            adjusted.timeout = std::chrono::seconds{0};
            if (adjusted.idle_timeout == std::chrono::seconds{0}) {
                adjusted.idle_timeout = std::chrono::seconds{120};
            }
            return dagent::llm::make_session(provider, adjusted, dagent::agent::RetryOptions{retries});
        };

        switch (args.mode) {
        case Mode::sessions:
        case Mode::models:
            break; // 查询模式已在上面的后端分支返回
        case Mode::interactive: {
            // 共享运行时只创建一次：环境事实只收集一次，MCP 连接全进程共用。
            auto assembly = dagent::app::Assembly::create(
                config.mcp_servers, config.mcp, dagent::workspace::collect_environment(args.cwd, {}),
                config.subagents, config.models, make_session);

            auto configuration = std::make_shared<dagent::app::Configuration>(
                paths.root, args.cwd, args.overrides, config, make_session);
            auto queries = std::make_shared<dagent::app::QueryGatewayImpl>(
                config.session, args.cwd, config.project_root, config.search);

            const dagent::agent::Setup setup = make_setup(config, args, assembly);
            dagent::app::SessionAssembly::Options factory_options;
            factory_options.assembly = assembly;
            factory_options.base = setup;
            factory_options.resolve_model = [configuration](const std::string& name) {
                return configuration->resolve(name);
            };

            dagent::runtime::Runtime::Deps deps;
            deps.configuration = configuration;
            deps.queries = queries;
            deps.factory = std::make_unique<dagent::app::SessionAssembly>(std::move(factory_options));
            deps.interactive = true;
            dagent::runtime::Runtime runtime(std::move(deps));
            dagent::runtime::StartResult started = runtime.start({args.resume_id, args.continue_last});

            dagent::ui::InteractiveOptions options;
            options.initial_prompt = args.prompt;
            options.resumed = started.resumed;
            options.replay = std::move(started.replay);
            return dagent::ui::run_interactive(runtime, options, interrupts);
        }
        case Mode::run: {
            auto assembly = dagent::app::Assembly::create(
                config.mcp_servers, config.mcp, dagent::workspace::collect_environment(args.cwd, {}),
                config.subagents, config.models, make_session);
            dagent::agent::HeadlessOptions options;
            options.prompt = args.prompt;
            options.resume_id = args.resume_id;
            options.continue_last = args.continue_last;
            switch (args.output) {
            case dagent::app::OutputFormat::json: options.output = dagent::agent::HeadlessOptions::Output::json; break;
            case dagent::app::OutputFormat::jsonl: options.output = dagent::agent::HeadlessOptions::Output::jsonl; break;
            case dagent::app::OutputFormat::text: options.output = dagent::agent::HeadlessOptions::Output::text; break;
            }
            return dagent::agent::run_headless(make_setup(config, args, assembly), options, interrupts);
        }
        }
    } catch (const dagent::app::ConfigError& error) {
        std::cerr << "config error: " << error.what() << "\n";
        return 2;
    } catch (const dagent::client::RpcFailure& error) {
        const bool config_error = error.error().kind == dagent::protocol::error_kind::kConfigError;
        std::cerr << (config_error ? "config error: " : "startup failed: ") << error.what() << "\n";
        return config_error ? 2 : 1;
    } catch (const std::exception& error) {
        std::cerr << "startup failed: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
