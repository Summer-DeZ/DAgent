#include "agent/catalog.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "agent/conversation.hpp"

namespace dagent::agent {
namespace {

/// 内置普通工具的固定顺序；其余普通工具（MCP 等动态追加）保持注册顺序排在控制动作之后。
constexpr std::string_view kOrderedBuiltins[] = {"read", "write", "edit", "bash", "grep", "glob"};

std::string join_names(const std::vector<ToolSpec>& specs) {
    std::string names;
    for (const ToolSpec& spec : specs) {
        if (!names.empty()) names += "、";
        names += spec.name;
    }
    return names;
}

} // namespace

ActionCatalog::ActionCatalog(ToolSession& tools, Config config)
    : tools_(tools), config_(std::move(config)),
      control_specs_(control_action_specs(config_.include_task, config_.subagents)) {}

bool ActionCatalog::control_enabled(std::string_view name) const {
    if (config_.allowed_tools.empty()) return true;
    return std::ranges::find(config_.allowed_tools, name) != config_.allowed_tools.end();
}

std::vector<ToolSpec> ActionCatalog::specs() const {
    std::vector<ToolSpec> ordinary = tools_.specs();
    std::vector<bool> used(ordinary.size(), false);
    const auto take = [&](std::string_view name) -> const ToolSpec* {
        for (std::size_t i = 0; i < ordinary.size(); ++i) {
            if (!used[i] && ordinary[i].name == name) {
                used[i] = true;
                return &ordinary[i];
            }
        }
        return nullptr;
    };

    std::vector<ToolSpec> out;
    out.reserve(ordinary.size() + control_specs_.size());
    for (const std::string_view name : kOrderedBuiltins) {
        if (const ToolSpec* spec = take(name)) out.push_back(*spec);
    }
    for (const ToolSpec& spec : control_specs_) {
        if (control_enabled(spec.name)) out.push_back(spec);
    }
    for (std::size_t i = 0; i < ordinary.size(); ++i) {
        if (!used[i]) out.push_back(std::move(ordinary[i]));
    }
    return out;
}

std::vector<std::string> ActionCatalog::names() const {
    std::vector<std::string> names;
    for (const ToolSpec& spec : specs()) names.push_back(spec.name);
    return names;
}

bool ActionCatalog::contains(std::string_view name) const {
    if (std::ranges::any_of(control_specs_, [&](const ToolSpec& spec) {
            return spec.name == name && control_enabled(name);
        })) return true;
    const std::vector<ToolSpec> ordinary = tools_.specs();
    return std::ranges::any_of(ordinary, [&](const ToolSpec& spec) { return spec.name == name; });
}

std::expected<PreparedAction, ToolResult> ActionCatalog::prepare(std::string_view name,
                                                                 std::string_view arguments,
                                                                 const InvocationContext& invocation) const {
    const auto control = std::ranges::find_if(control_specs_, [&](const ToolSpec& spec) {
        return spec.name == name;
    });
    if (control != control_specs_.end() && control_enabled(name)) {
        auto request = parse_control_action(name, arguments, invocation, config_.subagents);
        if (!request) return std::unexpected(std::move(request.error()));
        return PreparedAction{std::move(*request)};
    }

    const std::vector<ToolSpec> ordinary = tools_.specs();
    const bool known = std::ranges::any_of(ordinary, [&](const ToolSpec& spec) {
        return spec.name == name;
    });
    if (!known) {
        ToolResult result;
        result.model_text = std::format(texts::kUnknownTool, name, join_names(specs()));
        result.is_error = true;
        return std::unexpected(std::move(result));
    }

    auto prepared = tools_.prepare(name, arguments, invocation);
    if (!prepared) return std::unexpected(std::move(prepared.error()));
    return PreparedAction{std::move(*prepared)};
}

} // namespace dagent::agent
