#include "exec/detail.hpp"


#include <algorithm>
#include <string>

namespace dagent::exec::detail {
namespace {

bool is_denied(std::string_view name, const std::vector<std::string>& patterns) {
    for (const auto& pattern : patterns) {
        if (environment_name_matches(name, pattern)) return true;
    }
    return false;
}

// 环境条目形如 "名字=值"；名字为空或没有 '=' 的条目直接忽略。
bool entry_has_name(const std::string& entry, std::string_view name) {
    const auto equals = entry.find('=');
    return equals != std::string::npos &&
           environment_names_equal(std::string_view(entry).substr(0, equals), name);
}

void set_entry(std::vector<std::string>& env, std::string_view name, std::string_view value) {
    std::string entry = std::string(name) + "=" + std::string(value);
    for (auto& existing : env) {
        if (entry_has_name(existing, name)) {
            existing = std::move(entry);
            return;
        }
    }
    env.push_back(std::move(entry));
}

void remove_entry(std::vector<std::string>& env, std::string_view name) {
    std::erase_if(env, [&](const std::string& entry) { return entry_has_name(entry, name); });
}

// 默认注入的变量：防止命令进分页器、停下来等输入、吐颜色转义。
// 调用方可以通过 env_unset 撤掉其中任意一个，通过 env_set 覆盖成别的值。
constexpr std::pair<std::string_view, std::string_view> kDefaultEnv[] = {
    {"PAGER", "cat"}, {"GIT_PAGER", "cat"}, {"GIT_TERMINAL_PROMPT", "0"},
    {"TERM", "dumb"}, {"NO_COLOR", "1"},
};

} // namespace

std::vector<std::string> make_environment(const Command& cmd, const Options& opt) {
    std::vector<std::string> env;
    if (cmd.inherit_env) {
        env = inherited_environment();
        std::erase_if(env, [&](const std::string& entry) {
            const std::string_view view(entry);
            const auto equals = view.find('=');
            return equals == std::string_view::npos || equals == 0 ||
                   is_denied(view.substr(0, equals), opt.env_deny);
        });
    }

    for (const auto& [name, value] : kDefaultEnv) set_entry(env, name, value);
    for (const auto& [name, value] : opt.environment) set_entry(env, name, value);
    for (const auto& name : cmd.env_unset) remove_entry(env, name);
    for (const auto& [name, value] : cmd.env_set) set_entry(env, name, value);
    return env;
}

std::string_view find_env(const std::vector<std::string>& env, std::string_view name) {
    for (const auto& entry : env) {
        if (entry_has_name(entry, name)) return std::string_view(entry).substr(entry.find('=') + 1);
    }
    return {};
}

} // namespace dagent::exec::detail
