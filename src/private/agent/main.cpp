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
#include "agent/options.hpp"
#include "agent/record.hpp"
#include "app/cli.hpp"
#include "app/config.hpp"
#include "base/log.hpp"
#include "exec/sandbox.hpp"
#include "tui/grapheme.hpp"
#include "ui/shell.hpp"
#include "workspace/files.hpp"

namespace {

namespace fs = std::filesystem;
using dagent::app::Mode;

// docs/design/agent.md §12：整个进程忽略 SIGPIPE，写关闭的管道得到 EPIPE 而不是被信号杀死。
// exec 层只会在 SIGPIPE 仍是默认处理时设置它，两者不冲突。
void ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); }

dagent::agent::PermissionMode
permission_mode(const dagent::app::Config& config, const dagent::app::Args& args) {
    // 优先级：--permissions → 配置的 permissions → automatic（docs/design/agent.md §7）。
    if (args.permissions) {
        return *args.permissions == "deny" ? dagent::agent::PermissionMode::deny
                                           : dagent::agent::PermissionMode::automatic;
    }
    return config.agent.permissions;
}

dagent::agent::Setup make_setup(const dagent::app::Config& config, const dagent::app::Args& args) {
    namespace agent = dagent::agent;

    agent::Setup setup;
    setup.options = config.agent;
    setup.provider = config.models.at(config.model);

    setup.http = config.http;
    setup.http.timeout = std::chrono::seconds{0};
    if (setup.http.idle_timeout == std::chrono::seconds{0}) {
        setup.http.idle_timeout = std::chrono::seconds{120};
    }

    setup.cwd = args.cwd;
    setup.project_root = config.project_root;
    std::error_code ec;
    if (fs::exists(config.project_root / ".git", ec)) setup.git_root = config.project_root;

    setup.tools = config.tools;
    setup.files = config.files;
    setup.search = config.search;
    setup.process = config.process;
    setup.session = config.session;
    setup.mcp = config.mcp;
    setup.mcp_servers = config.mcp_servers;

    setup.sandbox = dagent::exec::probe();
    setup.permission_mode = permission_mode(config, args);
    if (!config.system_prompt_file.empty()) {
        dagent::workspace::TextFile file =
            dagent::workspace::read_text(config.system_prompt_file, config.files);
        setup.system_prompt_override = std::move(file.content);
    }
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

void print_sessions(const dagent::app::Config& config) {
    const std::vector<dagent::session::Summary> sessions = dagent::session::list(
        config.session, config.project_root, 20, dagent::agent::session_title);
    if (sessions.empty()) {
        std::cout << "no sessions for this project\n";
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

    if (args.mode == Mode::trust) {
        const fs::path root = dagent::app::project_root(args.cwd);
        try {
            dagent::app::trust_project(root);
        } catch (const std::exception& error) {
            std::cerr << "failed to trust: " << error.what() << "\n";
            return 1;
        }
        std::cout << "trusted " << root.string() << "\n";
        return 0;
    }

    try {
        const dagent::base::Secrets secrets =
            dagent::app::load_secrets(dagent::app::project_root(args.cwd));
        dagent::app::Config config =
            dagent::app::load_config({args.cwd, args.config_file, args.overrides, std::nullopt}, secrets);
        if (args.log_level) config.log.level = *args.log_level;
        // 全屏界面下写 stderr 会弄花画面；run 模式按 log.also_stderr 配置。
        if (args.mode == Mode::interactive) config.log.also_stderr = false;
        dagent::base::init_log(config.log);
        for (const auto& note : config.model_selection_log) dagent::base::logger("app")->info("{}", note);

        switch (args.mode) {
        case Mode::models:
            std::cout << "NAME\tKIND\tMODEL\tBASE_URL\tKEY\n";
            for (const auto& [name, model] : config.models)
                std::cout << name << (name == config.model ? " *" : "") << '\t' << model.kind << '\t'
                          << model.model << '\t' << model.base_url << '\t'
                          << (model.api_key.empty() ? "no" : "yes") << '\n';
            return 0;
        case Mode::sessions:
            print_sessions(config);
            return 0;
        case Mode::interactive: {
            if (!config.untrusted_files.empty()) {
                std::cout << "These project config files are untrusted and were ignored:\n";
                for (const auto& file : config.untrusted_files) std::cout << "  " << file.string() << '\n';
                std::cout << "They can change the model gateway and run arbitrary commands. Trust them only if you know where they came from.\n"
                          <<  "Trust " << config.project_root.string() << "? [y/N] " << std::flush;
                std::string answer;
                std::getline(std::cin, answer);
                if (answer == "y" || answer == "Y") {
                    dagent::app::trust_project(config.project_root);
                    config = dagent::app::load_config(
                        {args.cwd, args.config_file, args.overrides, std::nullopt}, secrets);
                    if (args.log_level) config.log.level = *args.log_level;
                    config.log.also_stderr = false;
                    dagent::base::init_log(config.log);
                    for (const auto& note : config.model_selection_log) dagent::base::logger("app")->info("{}", note);
                }
            }
            dagent::ui::InteractiveOptions options;
            options.models = config.models;
            options.resolve_model = [args](const std::string& name) {
                const auto secrets = dagent::app::load_secrets(dagent::app::project_root(args.cwd));
                auto overrides = args.overrides;
                overrides.push_back("@model=" + name);
                auto config = dagent::app::load_config({args.cwd, args.config_file, overrides, std::nullopt}, secrets);
                for (const auto& note : config.model_selection_log) dagent::base::logger("app")->info("{}", note);
                return config.models.at(name);
            };
            options.initial_prompt = args.prompt;
            options.resume_id = args.resume_id;
            options.continue_last = args.continue_last;
            if (!config.ui.theme_file.empty()) options.theme_file = config.ui.theme_file;
            return dagent::ui::run_interactive(make_setup(config, args), options, interrupts);
        }
        case Mode::trust:
            break; // 上面已经处理
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
            return dagent::agent::run_headless(make_setup(config, args), options, interrupts);
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
