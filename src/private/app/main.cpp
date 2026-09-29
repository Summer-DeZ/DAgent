#include <chrono>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <variant>

#include <signal.h>

#include "app/cli.hpp"
#include "app/interrupts.hpp"
#include "app/launcher.hpp"
#include "app/paths.hpp"
#include "app/run.hpp"
#include "client/client.hpp"
#include "protocol/dto.hpp"
#include "tui/grapheme.hpp"
#include "ui/shell.hpp"

namespace {

namespace fs = std::filesystem;
using dagent::app::Mode;

// docs/design/app.md §6：整个进程忽略 SIGPIPE，写关闭的管道得到 EPIPE 而不是被信号杀死。
// exec 层只会在 SIGPIPE 仍是默认处理时设置它，两者不冲突。
void ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); }

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
    dagent::app::Interrupts& interrupts = dagent::app::install_interrupts();
    ignore_sigpipe();

    const std::variant<dagent::app::Args, int> parsed = dagent::app::parse_args(argc, argv);
    if (const int* code = std::get_if<int>(&parsed)) return *code;
    const dagent::app::Args& args = std::get<dagent::app::Args>(parsed);

    try {
        const dagent::app::HomePaths paths = dagent::app::home_paths();

        if (args.mode == Mode::runtime_sync || args.mode == Mode::runtime_list ||
            args.mode == Mode::sandbox_status) {
            dagent::app::BackendLaunch launch;
            launch.root = paths.root;
            launch.cwd = args.cwd;
            launch.mode = args.mode == Mode::runtime_sync    ? "runtime-sync"
                          : args.mode == Mode::runtime_list  ? "runtime-list"
                                                             : "sandbox-status";
            auto session = dagent::app::BackendSession::start(launch, {});
            std::cout << session.initialized().at("maintenance").dump(2) << '\n';
            return 0;
        }

        // 查询模式：启动本前端独占的后端，经私有协议完成初始化与查询。
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

        // run 模式：启动本前端独占的后端，提示词经 input.submit 交给后端执行。
        if (args.mode == Mode::run) {
            dagent::app::BackendLaunch launch;
            launch.root = paths.root;
            launch.cwd = args.cwd;
            launch.overrides = args.overrides;
            launch.mode = "run";
            launch.permissions = args.permissions;
            launch.read_only = args.read_only;
            launch.plan = args.plan;
            launch.resume_id = args.resume_id;
            launch.continue_last = args.continue_last;
            launch.log_level = args.log_level;
            return dagent::app::run_backend(launch, args.prompt, args.output, interrupts);
        }

        // 交互模式：前端只持 Client、只读 DTO 与页面状态。
        dagent::ui::FrontendBridge bridge;
        dagent::client::Client::Callbacks callbacks;
        callbacks.event = [&bridge](const dagent::protocol::Event& event) { bridge.event(event); };
        callbacks.interaction = [&bridge](const dagent::protocol::InteractionRequest& request) {
            bridge.interaction_requested(request);
        };
        callbacks.interaction_closed = [&bridge](const std::string& id) {
            bridge.interaction_closed(id);
        };
        callbacks.disconnected = [&bridge](const std::string& message) {
            bridge.disconnected(message);
        };

        dagent::app::BackendLaunch launch;
        launch.root = paths.root;
        launch.cwd = args.cwd;
        launch.overrides = args.overrides;
        launch.mode = "interactive";
        launch.permissions = args.permissions;
        launch.read_only = args.read_only;
        launch.plan = args.plan;
        launch.resume_id = args.resume_id;
        launch.continue_last = args.continue_last;
        launch.log_level = args.log_level;

        dagent::app::BackendSession session =
            dagent::app::BackendSession::start(launch, std::move(callbacks));
        const nlohmann::json& initialized = session.initialized();
        const nlohmann::json session_json = initialized.value("session", nlohmann::json(nullptr));
        if (!session_json.is_object()) {
            std::cerr << "startup failed: the backend did not create a session\n";
            return 1;
        }
        dagent::ui::InteractiveOptions options;
        options.initial_prompt = args.prompt;
        options.resumed = initialized.value("resumed", false);
        options.initial = session_json.get<dagent::protocol::SessionSnapshot>();
        if (const auto ui = initialized.find("ui"); ui != initialized.end() && ui->is_object()) {
            if (const auto theme = ui->find("theme_file"); theme != ui->end() && theme->is_string()) {
                options.theme_file = theme->get<std::string>();
            }
        }

        interrupts.graceful = true;
        const int code = dagent::ui::run_interactive(session.client(), bridge, options,
                                                     interrupts.stop.get_token());
        interrupts.graceful = false;
        return code;
    } catch (const dagent::app::InstallError& error) {
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
