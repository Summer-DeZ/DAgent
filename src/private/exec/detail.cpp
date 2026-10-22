#include "exec/detail.hpp"

#include "base/text.hpp"

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

OutputCollector::OutputCollector(std::size_t max_bytes)
    : head_cap_(max_bytes / 2), tail_cap_(max_bytes - max_bytes / 2) {}

void OutputCollector::append(std::string_view chunk) {
    total_ += chunk.size();
    if (head_.size() < head_cap_) {
        const std::size_t take = std::min(head_cap_ - head_.size(), chunk.size());
        head_.append(chunk.substr(0, take));
        chunk.remove_prefix(take);
    }
    if (chunk.empty()) return;
    tail_.append(chunk);
    if (tail_.size() > tail_cap_) tail_.erase(0, tail_.size() - tail_cap_);
}

std::string OutputCollector::finish() const {
    if (total_ <= head_cap_ + tail_cap_) return head_ + tail_;

    const std::size_t head_end = base::utf8_floor(head_, head_.size());
    std::size_t tail_skip = 0;
    while (tail_skip < tail_.size() && (static_cast<unsigned char>(tail_[tail_skip]) & 0xC0) == 0x80)
        ++tail_skip;
    const std::size_t omitted = total_ - head_end - (tail_.size() - tail_skip);

    std::string text = head_.substr(0, head_end);
    text += "\n... " + std::to_string(omitted) + " bytes omitted ...\n";
    text += tail_.substr(tail_skip);
    return text;
}

} // namespace dagent::exec::detail
