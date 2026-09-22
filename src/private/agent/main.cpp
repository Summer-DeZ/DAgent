#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include <signal.h>

#include "agent/headless.hpp"
#include "agent/host.hpp"
#include "agent/options.hpp"
#include "agent/record.hpp"
#include "app/cli.hpp"
#include "app/config.hpp"
#include "base/log.hpp"
#include "exec/sandbox.hpp"
#include "llm/llm.hpp"
#include "tui/grapheme.hpp"
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
                                const std::shared_ptr<dagent::agent::AgentHost>& host) {
    namespace agent = dagent::agent;

    agent::Setup setup;
    setup.options = config.agent;
    const ProviderConfig& selected = config.models.at(config.model);
    setup.provider = dagent::llm::to_public(selected);
    setup.model_session = host->make_model_session(selected);

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
    setup.host = host;
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

void print_sessions(const dagent::app::Config& config, const fs::path& cwd) {
    const std::vector<dagent::session::Summary> sessions =
        dagent::session::list(config.session, cwd, 20);
    if (sessions.empty()) {
        std::cout << "no sessions for this working directory\n";
        return;
    }
    for (const dagent::session::Summary& summary : sessions) {
        auto [title, width] = fit_title(summary.title, 50);
        std::cout << local_minute(summary.updated) << "   " << title
                  << std::string(static_cast<std::size_t>(50 - width), ' ') << "   "
                  << summary.meta.id << '\n';
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

        // 共享运行时只创建一次：环境事实只收集一次，MCP 连接全进程共用，审批在 host 里串行化。
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
        auto host = dagent::agent::AgentHost::create(
            config.mcp_servers, config.mcp, dagent::workspace::collect_environment(args.cwd, {}),
            config.subagents, config.models, make_session);

        switch (args.mode) {
        case Mode::models:
            std::cout << "NAME\tKIND\tMODEL\tBASE_URL\tKEY\n";
            for (const auto& [name, model] : config.models)
                std::cout << name << (name == config.model ? " *" : "") << '\t' << model.kind << '\t'
                          << model.model << '\t' << model.base_url << '\t'
                          << (model.api_key.empty() ? "no" : "yes") << '\n';
            return 0;
        case Mode::sessions:
            print_sessions(config, args.cwd);
            return 0;
        case Mode::interactive: {
            dagent::ui::InteractiveOptions options;
            for (const auto& [name, model] : config.models) options.models.emplace(name, dagent::llm::to_public(model));
            for (const dagent::llm::ProviderInfo& info : dagent::llm::providers()) {
                options.provider_kinds.push_back({std::string(info.kind), std::string(info.default_base_url),
                                                  info.needs_api_key});
            }
            const auto selection = [host](const ProviderConfig& provider) {
                return dagent::ui::ModelSelection{dagent::llm::to_public(provider),
                                                  host->make_model_session(provider)};
            };
            options.resolve_model = [args, root = paths.root, selection](const std::string& name) {
                auto overrides = args.overrides;
                overrides.push_back("@model=" + name);
                auto config = dagent::app::load_config({root, args.cwd, overrides});
                for (const auto& note : config.model_selection_log) dagent::base::logger("app")->info("{}", note);
                return selection(config.models.at(name));
            };
            options.add_model = [root = paths.root, selection](dagent::agent::ModelInput input) {
                // 凭据只在此处进入内部配置；保存后立即以公开视图返回。
                ProviderConfig model;
                model.kind = std::move(input.kind);
                model.name = std::move(input.name);
                model.base_url = std::move(input.base_url);
                model.model = std::move(input.model);
                model.api_key = std::move(input.credential);
                model.max_tokens = input.max_tokens;
                model.context_window = input.context_window;
                return selection(dagent::app::add_model(root, model));
            };
            options.initial_prompt = args.prompt;
            options.resume_id = args.resume_id;
            options.continue_last = args.continue_last;
            if (!config.ui.theme_file.empty()) options.theme_file = config.ui.theme_file;
            return dagent::ui::run_interactive(make_setup(config, args, host), options, interrupts);
        }
        case Mode::run: {
            dagent::agent::HeadlessOptions options;
            options.prompt = args.prompt;
            options.resume_id = args.resume_id;
            options.continue_last = args.continue_last;
            switch (args.output) {
            case dagent::app::OutputFormat::json: options.output = dagent::agent::HeadlessOptions::Output::json; break;
            case dagent::app::OutputFormat::jsonl: options.output = dagent::agent::HeadlessOptions::Output::jsonl; break;
            case dagent::app::OutputFormat::text: options.output = dagent::agent::HeadlessOptions::Output::text; break;
            }
            return dagent::agent::run_headless(make_setup(config, args, host), options, interrupts);
        }
        }
    } catch (const dagent::app::ConfigError& error) {
        std::cerr << "config error: " << error.what() << "\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "startup failed: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
