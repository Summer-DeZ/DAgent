#include "base/log.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace dagent::base {
namespace {

struct State {
    std::recursive_mutex mutex; ///< 崩溃处理可能重入，用递归锁避免自锁
    bool initialized = false;
    bool terminate_installed = false;
    std::vector<spdlog::sink_ptr> sinks;
    spdlog::level::level_enum level = spdlog::level::info;
    std::unordered_map<std::string, spdlog::level::level_enum> module_levels;
};

State& state() {
    static State s;
    return s;
}

std::terminate_handler& previous_handler() {
    static std::terminate_handler handler = nullptr;
    return handler;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

spdlog::level::level_enum parse_level(std::string_view name) {
    const std::string text(trim(name));
    const auto level = spdlog::level::from_str(text);
    if (level == spdlog::level::off && text != "off")
        throw std::invalid_argument("unknown log level: " + text);
    return level;
}

// DAGENT_LOG 形如 "info" 或 "net=trace,info"：裸级别设置默认值，module=level 单独覆盖。
void parse_level_spec(std::string_view spec, spdlog::level::level_enum& default_level,
                      std::unordered_map<std::string, spdlog::level::level_enum>& module_levels) {
    while (!spec.empty()) {
        const auto comma = spec.find(',');
        const std::string_view token =
            trim(comma == std::string_view::npos ? spec : spec.substr(0, comma));
        spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
        if (token.empty()) continue;
        const auto eq = token.find('=');
        if (eq == std::string_view::npos) {
            default_level = parse_level(token);
        } else {
            const std::string_view name = trim(token.substr(0, eq));
            if (!name.empty()) module_levels[std::string(name)] = parse_level(token.substr(eq + 1));
        }
    }
}

std::shared_ptr<spdlog::logger> null_logger() {
    static const std::shared_ptr<spdlog::logger> instance = [] {
        auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        auto l = std::make_shared<spdlog::logger>("null", std::move(sink));
        l->set_level(spdlog::level::off);
        return l;
    }();
    return instance;
}

void on_terminate() {
    shutdown_log();
    if (auto handler = previous_handler()) handler();
    else std::abort();
}

void shutdown_locked() {
    // spdlog 1.17 的 shutdown() 只停掉定时刷盘线程、不再 flush，这里显式刷一次，
    // 否则缓冲在文件 sink 里、尚未到 warn 门槛的日志会丢。
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& l) { l->flush(); });
    spdlog::shutdown();
    // shutdown 会清空默认 logger，之后 spdlog::info 会直接解引用空指针；
    // 换成 null logger，退出后误用默认接口既不会崩溃也不会弄花终端。
    spdlog::set_default_logger(null_logger());
    auto& s = state();
    s.sinks.clear();
    s.module_levels.clear();
    s.initialized = false;
}

} // namespace

void init_log(const LogOptions& opt) {
    std::lock_guard lock(state().mutex);
    if (state().initialized) shutdown_locked();

    auto& s = state();
    s.level = parse_level(opt.level);
    if (const char* spec = std::getenv("DAGENT_LOG"); spec && *spec)
        parse_level_spec(spec, s.level, s.module_levels);

    if (opt.file.empty()) throw std::invalid_argument("log file path is empty");
    const std::filesystem::path file = opt.file;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());

    // spdlog 的 max_files 是备份文件数，备份加上当前文件才是总文件数，这里换算成总数上限。
    const std::size_t backups = opt.max_files > 0 ? opt.max_files - 1 : 0;
    s.sinks.push_back(
        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(file.string(), opt.max_file_bytes, backups));
    if (opt.also_stderr)
        s.sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());

    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [t%t] %v");
    spdlog::set_level(s.level);
    spdlog::flush_on(spdlog::level::warn); // 之后创建的 logger 都继承这个刷盘门槛
    spdlog::flush_every(std::chrono::seconds{1});

    // spdlog::info 这类默认接口也挂到同一组 sink 上，不能写 stdout/stderr。
    auto default_logger = std::make_shared<spdlog::logger>("dagent", s.sinks.begin(), s.sinks.end());
    spdlog::initialize_logger(default_logger);
    spdlog::set_default_logger(std::move(default_logger));

    s.initialized = true;
    if (!s.terminate_installed) { // 只装一次，避免重复 init 时把 on_terminate 自己串成上一个处理函数
        previous_handler() = std::set_terminate(on_terminate);
        s.terminate_installed = true;
    }
}

std::shared_ptr<spdlog::logger> logger(std::string_view module) {
    std::lock_guard lock(state().mutex);
    if (!state().initialized) return null_logger();

    const std::string name(module);
    if (auto existing = spdlog::get(name)) return existing;

    // 共用同一组 sink；initialize_logger 套用全局格式、级别与刷盘门槛并完成注册，
    // 之后再应用模块专属级别。
    auto l = std::make_shared<spdlog::logger>(name, state().sinks.begin(), state().sinks.end());
    spdlog::initialize_logger(l);
    if (auto it = state().module_levels.find(name); it != state().module_levels.end())
        l->set_level(it->second);
    return l;
}

void shutdown_log() {
    std::lock_guard lock(state().mutex);
    if (state().initialized) shutdown_locked();
}

} // namespace dagent::base
