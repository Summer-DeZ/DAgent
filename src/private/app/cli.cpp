#include "app/cli.hpp"

#include <cerrno>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <unistd.h>

#include <CLI/CLI.hpp>

namespace dagent::app {
namespace fs = std::filesystem;
namespace {

std::string read_stdin_all() {
    std::string data;
    char buffer[8192];
    for (;;) {
        const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof buffer);
        if (n > 0) {
            data.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    return data;
}

// 提示词没加引号时是多个位置参数：按空格拼回一句，而不是只取第一个词。
std::string join_words(const std::vector<std::string>& words) {
    std::string out;
    for (const auto& word : words) {
        if (!out.empty()) out += ' ';
        out += word;
    }
    return out;
}

std::string trim_trailing_newlines(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

fs::path make_absolute(const fs::path& path) {
    std::error_code ec;
    const fs::path result = fs::weakly_canonical(path, ec);
    return ec ? fs::absolute(path).lexically_normal() : result;
}

// CLI11 的选项回调按定义顺序执行，-m 和 --set 混用时拿不到真实的出现顺序；
// 这里按 argv 顺序单独扫一遍，保证「后面的覆盖前面的」。
std::vector<std::string> collect_overrides(int argc, char** argv) {
    std::vector<std::string> overrides;
    bool literal = false; // "--" 之后都是位置参数
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (literal) continue;
        if (arg == "--") {
            literal = true;
            continue;
        }
        if (arg == "--set" || arg == "-m" || arg == "--model") {
            if (i + 1 < argc) {
                const std::string value(argv[++i]);
                overrides.push_back(arg == "--set" ? value : "@model=" + value);
            }
            continue;
        }
        if (arg.starts_with("--set=")) {
            overrides.push_back(std::string(arg.substr(6)));
        } else if (arg.starts_with("--model=")) {
            overrides.push_back("@model=" + std::string(arg.substr(8)));
        } else if (arg.starts_with("-m") && arg.size() > 2) {
            overrides.push_back("@model=" + std::string(arg.substr(2)));
        }
    }
    return overrides;
}

} // namespace

std::variant<Args, int> parse_args(int argc, char** argv) {
    CLI::App app{"DAgent - a terminal coding agent", "dagent"};
    app.footer("Exit codes: 0 ok, 1 failed, 2 bad usage, 130 interrupted.");
    app.set_version_flag("--version", DAGENT_VERSION, "print version and exit");

    std::string cwd;
    app.add_option("-C,--cwd", cwd, "working directory (default: current)")->check(CLI::ExistingDirectory);
    std::vector<std::string> set_values;
    // allow_extra_args(false)：每次出现只取一个值、可以重复；否则后面的提示词会被当成它的值吞掉。
    app.add_option("--set", set_values, "override config: key=value, repeatable; keys are dot-separated (literal dots in keys are unsupported)")
        ->allow_extra_args(false);
    std::vector<std::string> model_values;
    app.add_option("-m,--model", model_values, "select a named configuration, or override the current model id")->allow_extra_args(false);
    bool list_models = false;
    app.add_flag("--list-models", list_models, "list named model configurations and exit");
    std::string resume;
    CLI::Option* resume_option = app.add_option("-r,--resume", resume, "resume a session by id");
    bool continue_last = false;
    CLI::Option* continue_option = app.add_flag("--continue", continue_last, "resume the latest session in this project");
    resume_option->excludes(continue_option);
    std::string log_level;
    app.add_option("--log-level", log_level, "log level");
    std::string permissions;
    app.add_option("--permissions", permissions, "permission mode: ask / workspace / unrestricted")
        ->check(CLI::IsMember({"ask", "workspace", "unrestricted"}));
    bool read_only = false;
    app.add_flag("--read-only", read_only, "reject writes and state-changing commands");
    bool plan = false;
    app.add_flag("--plan", plan, "start in read-only planning mode");
    std::vector<std::string> prompt_words;
    app.add_option("prompt", prompt_words, "open the interactive interface; use the optional prompt as the first message")
        ->type_name("PROMPT");

    CLI::App* run = app.add_subcommand("run", "non-interactive: run one turn and exit");
    run->fallthrough(); // 通用选项写在子命令后面也认
    std::vector<std::string> run_words;
    run->add_option("run_prompt", run_words, "prompt (also accepted via stdin)")->type_name("PROMPT");
    std::string output = "text";
    run->add_option("--output", output, "output format")
        ->check(CLI::IsMember({"text", "json", "jsonl"}))
        ->default_str("text");

    CLI::App* sessions = app.add_subcommand("sessions", "list recent sessions");
    sessions->fallthrough();

    // 最多一个子命令：进入子命令之后，提示词里再出现 run/sessions 也只是普通的词。
    app.require_subcommand(0, 1);

    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        std::cout << app.help() << std::flush;
        return 0;
    } catch (const CLI::CallForVersion&) {
        std::cout << "dagent " << app.version() << "\n";
        return 0;
    } catch (const CLI::ParseError& e) {
        std::cerr << "usage error: " << e.what() << "\nUse --help for usage.\n";
        return 2;
    }

    // `dagent 修一下 run 的测试` 这种没加引号的提示词里带了子命令名：CLI11 会切进子命令、丢掉前面的词，
    // 意图说不清，直接报错让用户加引号。
    if (!prompt_words.empty() && (run->parsed() || sessions->parsed())) {
        std::cerr << "prompt contains a subcommand name; quote the prompt, e.g. dagent \"" << join_words(prompt_words)
                  << " …\"\n";
        return 2;
    }

    Args args;
    if (run->parsed()) {
        args.mode = Mode::run;
        args.prompt = join_words(run_words);
    } else if (sessions->parsed()) {
        args.mode = Mode::sessions;
    } else {
        args.mode = Mode::interactive;
        args.prompt = join_words(prompt_words);
    }
    if (!permissions.empty()) args.permissions = permissions;
    args.read_only = read_only;
    args.plan = plan;
    if (output == "json") args.output = OutputFormat::json;
    else if (output == "jsonl") args.output = OutputFormat::jsonl;

    args.cwd = cwd.empty() ? fs::current_path() : make_absolute(cwd);
    args.overrides = collect_overrides(argc, argv);
    if (!resume.empty()) args.resume_id = resume;
    args.continue_last = continue_last;
    if (!log_level.empty()) args.log_level = log_level;

    if (args.mode == Mode::sessions && (args.resume_id || args.continue_last)) {
        std::cerr << "sessions cannot be combined with --resume / --continue\n";
        return 2;
    }

    if (list_models) { args.mode = Mode::models; return args; }

    const bool stdin_tty = ::isatty(STDIN_FILENO) != 0;
    if (args.mode == Mode::interactive) {
        if (!stdin_tty) {
            std::cerr << "interactive mode requires a terminal; use dagent run for non-interactive use\n";
            return 2;
        }
    } else if (args.mode == Mode::run && !stdin_tty) {
        // 支持 git diff | dagent run "审查这个改动"
        const std::string from_stdin = trim_trailing_newlines(read_stdin_all());
        if (!from_stdin.empty()) {
            if (!args.prompt.empty()) args.prompt += "\n\n";
            args.prompt += from_stdin;
        }
    }
    if (args.mode == Mode::run && args.prompt.empty()) {
        std::cerr << "run requires a prompt: dagent run \"<prompt>\", or provide it via stdin\n";
        return 2;
    }
    return args;
}

} // namespace dagent::app
