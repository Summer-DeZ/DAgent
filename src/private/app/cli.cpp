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
                overrides.push_back(arg == "--set" ? value : "gateway.model=" + value);
            }
            continue;
        }
        if (arg.starts_with("--set=")) {
            overrides.push_back(std::string(arg.substr(6)));
        } else if (arg.starts_with("--model=")) {
            overrides.push_back("gateway.model=" + std::string(arg.substr(8)));
        } else if (arg.starts_with("-m") && arg.size() > 2) {
            overrides.push_back("gateway.model=" + std::string(arg.substr(2)));
        }
    }
    return overrides;
}

} // namespace

std::variant<Args, int> parse_args(int argc, char** argv) {
    CLI::App app{"DAgent —— 终端 Agent", "dagent"};
    app.footer("退出码：0 成功；1 运行失败；2 参数错误；130 被 Ctrl+C 中断。");
    app.set_version_flag("--version", DAGENT_VERSION, "显示版本并退出");

    std::string cwd;
    app.add_option("-C,--cwd", cwd, "工作目录（默认当前目录）")->check(CLI::ExistingDirectory);
    std::string config_file;
    app.add_option("-c,--config", config_file, "显式配置文件")->check(CLI::ExistingFile);
    std::vector<std::string> set_values;
    // allow_extra_args(false)：每次出现只取一个值、可以重复；否则后面的提示词会被当成它的值吞掉。
    app.add_option("--set", set_values, "覆盖配置：键=值，可重复；键按 . 分段，不能指定本身含 . 的键名")
        ->allow_extra_args(false);
    std::vector<std::string> model_values;
    app.add_option("-m,--model", model_values, "等价于 --set gateway.model=<模型>")->allow_extra_args(false);
    std::string resume;
    CLI::Option* resume_option = app.add_option("-r,--resume", resume, "恢复指定会话");
    bool continue_last = false;
    CLI::Option* continue_option = app.add_flag("--continue", continue_last, "恢复本项目最近的一次会话");
    resume_option->excludes(continue_option);
    std::string log_level;
    app.add_option("--log-level", log_level, "日志级别");
    std::vector<std::string> prompt_words;
    app.add_option("prompt", prompt_words, "进入交互界面；给了提示词就把它作为第一条消息")
        ->type_name("提示词");

    CLI::App* run = app.add_subcommand("run", "非交互：跑完一轮后退出");
    run->fallthrough(); // 通用选项写在子命令后面也认
    std::vector<std::string> run_words;
    run->add_option("run_prompt", run_words, "提示词（也可以从 stdin 传入）")->type_name("提示词");
    std::string permissions;
    run->add_option("--permissions", permissions, "没有人审批时的策略：auto / deny")
        ->check(CLI::IsMember({"auto", "deny"}));
    std::string output = "text";
    run->add_option("--output", output, "输出格式")
        ->check(CLI::IsMember({"text", "json", "jsonl"}))
        ->default_str("text");

    CLI::App* sessions = app.add_subcommand("sessions", "列出最近的会话");
    sessions->fallthrough();

    CLI::App* trust = app.add_subcommand("trust", "信任这个目录所在的项目：读取它的 .dagent/config.json 与 .mcp.json");
    trust->fallthrough();
    std::string trust_dir;
    trust->add_option("dir", trust_dir, "要信任的目录（默认当前目录；按它所在的 git 根记录）")
        ->check(CLI::ExistingDirectory)
        ->type_name("目录");

    // 最多一个子命令：进入子命令之后，提示词里再出现 run/sessions/trust 也只是普通的词。
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
        std::cerr << "参数错误：" << e.what() << "\n用 --help 查看用法。\n";
        return 2;
    }

    // `dagent 修一下 run 的测试` 这种没加引号的提示词里带了子命令名：CLI11 会切进子命令、丢掉前面的词，
    // 意图说不清，直接报错让用户加引号。
    if (!prompt_words.empty() && (run->parsed() || sessions->parsed() || trust->parsed())) {
        std::cerr << "提示词里出现了子命令名；请给提示词加引号，例如 dagent \"" << join_words(prompt_words)
                  << " …\"\n";
        return 2;
    }

    Args args;
    if (run->parsed()) {
        args.mode = Mode::run;
        args.prompt = join_words(run_words);
    } else if (sessions->parsed()) {
        args.mode = Mode::sessions;
    } else if (trust->parsed()) {
        args.mode = Mode::trust;
    } else {
        args.mode = Mode::interactive;
        args.prompt = join_words(prompt_words);
    }
    if (!permissions.empty()) args.permissions = permissions;
    if (output == "json") args.output = OutputFormat::json;
    else if (output == "jsonl") args.output = OutputFormat::jsonl;

    args.cwd = cwd.empty() ? fs::current_path() : make_absolute(cwd);
    if (!trust_dir.empty()) args.cwd = make_absolute(trust_dir);
    if (!config_file.empty()) args.config_file = make_absolute(config_file);
    args.overrides = collect_overrides(argc, argv);
    if (!resume.empty()) args.resume_id = resume;
    args.continue_last = continue_last;
    if (!log_level.empty()) args.log_level = log_level;

    if ((args.mode == Mode::sessions || args.mode == Mode::trust) &&
        (args.resume_id || args.continue_last)) {
        std::cerr << "sessions / trust 不能和 --resume / --continue 一起使用\n";
        return 2;
    }

    const bool stdin_tty = ::isatty(STDIN_FILENO) != 0;
    if (args.mode == Mode::interactive) {
        if (!stdin_tty) {
            std::cerr << "交互模式需要终端；非交互场景请使用 dagent run\n";
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
        std::cerr << "run 需要提示词：dagent run \"<提示词>\"，或者从 stdin 传入\n";
        return 2;
    }
    return args;
}

} // namespace dagent::app
