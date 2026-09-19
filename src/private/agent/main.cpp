#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "agent/headless.hpp"
#include "agent/options.hpp"
#include "app/cli.hpp"
#include "app/config.hpp"
#include "base/log.hpp"
#include "exec/sandbox.hpp"
#include "workspace/files.hpp"

namespace {

namespace fs = std::filesystem;
using dagent::app::Mode;

dagent::agent::PermissionMode parse_permission_mode(std::string_view value) {
    if (value == "deny") return dagent::agent::PermissionMode::deny;
    return dagent::agent::PermissionMode::automatic;
}

dagent::agent::Setup make_setup(const dagent::app::Config& config, const dagent::app::Args& args) {
    namespace agent = dagent::agent;

    agent::Setup setup;
    setup.options = config.agent;
    setup.model.model = config.gateway.model;
    setup.model.max_tokens = static_cast<std::size_t>(std::max(config.gateway.max_tokens, 0));
    setup.model.temperature = config.gateway.temperature.value_or(-1.0);
    setup.codec.base_url = config.gateway.base_url;
    setup.codec.api_key = config.gateway.api_key;
    setup.codec.send_reasoning_content = config.gateway.send_reasoning_content;
    setup.codec.include_usage = config.gateway.include_usage;
    setup.codec.extra_body = config.gateway.extra_body;

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
    setup.permission_mode = parse_permission_mode(args.permissions);
    if (!config.gateway.system_prompt_file.empty()) {
        dagent::workspace::TextFile file =
            dagent::workspace::read_text(config.gateway.system_prompt_file, config.files);
        setup.system_prompt_override = std::move(file.content);
    }
    return setup;
}

} // namespace

int main(int argc, char** argv) {
    const std::variant<dagent::app::Args, int> parsed = dagent::app::parse_args(argc, argv);
    if (const int* code = std::get_if<int>(&parsed)) return *code;
    const dagent::app::Args& args = std::get<dagent::app::Args>(parsed);

    if (args.mode == Mode::trust) {
        const fs::path root = dagent::app::project_root(args.cwd);
        try {
            dagent::app::trust_project(root);
        } catch (const std::exception& error) {
            std::cerr << "信任失败：" << error.what() << "\n";
            return 1;
        }
        std::cout << "已信任 " << root.string() << "\n";
        return 0;
    }

    try {
        const dagent::base::Secrets secrets =
            dagent::app::load_secrets(dagent::app::project_root(args.cwd));
        dagent::app::Config config =
            dagent::app::load_config({args.cwd, args.config_file, args.overrides, std::nullopt}, secrets);
        if (args.log_level) config.log.level = *args.log_level;
        dagent::base::init_log(config.log);

        switch (args.mode) {
        case Mode::sessions:
            std::cerr << "sessions 子命令尚未实现\n";
            return 2;
        case Mode::interactive:
            std::cerr << "交互界面尚未实现；请使用 dagent run\n";
            return 2;
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
            return dagent::agent::run_headless(make_setup(config, args), options);
        }
        }
    } catch (const dagent::app::ConfigError& error) {
        std::cerr << "配置错误：" << error.what() << "\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "启动失败：" << error.what() << "\n";
        return 1;
    }
    return 0;
}
