#include <format>
#include <utility>

#include "tools/detail.hpp"

namespace dagent::tools {
namespace {

class InteractiveCall final : public Call {
public:
    InteractiveCall(agent::ToolKind kind, agent::AskView view, std::string summary = {}) {
        intent_.kind = kind;
        intent_.ask = std::move(view);
        intent_.plan_summary = std::move(summary);
        intent_.summary = kind == agent::ToolKind::exit_plan ? "Submit implementation plan"
                                                          : "Ask the user: " + intent_.ask.prompt;
    }

private:
    Result do_run(const Grant&, const std::function<void(std::string_view)>&,
                  std::stop_token) override {
        return detail::error_result("interactive calls are handled by the agent dispatcher");
    }
};

class AskTool final : public Tool {
public:
    AskTool() {
        spec_.name = "ask";
        spec_.description =
            "Ask the user to choose only when different choices would materially change the work. "
            "Do not ask for facts you can discover yourself. Provide 2 to 4 concise options.";
        spec_.parameters = {
            {"type", "object"},
            {"properties", {
                {"header", {{"type", "string"}, {"description", "Short dialog title"}}},
                {"prompt", {{"type", "string"}, {"description", "The decision to make"}}},
                {"options", {{"type", "array"}, {"minItems", 2}, {"maxItems", 4},
                    {"items", {{"type", "object"}, {"properties", {
                        {"label", {{"type", "string"}}},
                        {"description", {{"type", "string"}}}
                    }}, {"required", std::vector<std::string>{"label", "description"}},
                    {"additionalProperties", false}}}}},
                {"multi_select", {{"type", "boolean"}, {"default", false}}},
                {"allow_other", {{"type", "boolean"}, {"default", true}}}
            }},
            {"required", std::vector<std::string>{"header", "prompt", "options"}},
            {"additionalProperties", false}
        };
    }

    const Spec& spec() const override { return spec_; }

    std::expected<std::unique_ptr<Call>, Result> prepare(std::string_view arguments,
                                                         Context&) const override {
        auto args = detail::parse_arguments(arguments);
        if (!args) return std::unexpected(detail::error_result(args.error()));
        std::string error;
        agent::AskView view;
        view.header = detail::require_string(*args, "header", error);
        view.prompt = detail::require_string(*args, "prompt", error);
        const auto multi = detail::get_bool(*args, "multi_select", error);
        const auto other = detail::get_bool(*args, "allow_other", error);
        if (!error.empty()) return std::unexpected(detail::error_result(error));
        view.multi_select = multi.value_or(false);
        view.allow_other = other.value_or(true);

        const auto options = args->find("options");
        if (options == args->end() || !options->is_array() || options->size() < 2 || options->size() > 4)
            return std::unexpected(detail::error_result("options must contain 2 to 4 entries"));
        for (const auto& entry : *options) {
            if (!entry.is_object()) return std::unexpected(detail::error_result("each option must be an object"));
            const auto label = entry.find("label");
            const auto description = entry.find("description");
            if (label == entry.end() || !label->is_string() || label->get_ref<const std::string&>().empty() ||
                description == entry.end() || !description->is_string()) {
                return std::unexpected(detail::error_result(
                    "each option needs a non-empty label and string description"));
            }
            view.options.push_back({label->get<std::string>(), description->get<std::string>()});
        }
        return std::make_unique<InteractiveCall>(agent::ToolKind::ask, std::move(view));
    }

private:
    Spec spec_;
};

class ExitPlanTool final : public Tool {
public:
    ExitPlanTool() {
        spec_.name = "exit_plan";
        spec_.description = "Submit a complete implementation plan for user approval. Use only in planning mode.";
        spec_.parameters = {
            {"type", "object"},
            {"properties", {{"summary", {{"type", "string"}, {"description", "Complete proposed plan"}}}}},
            {"required", std::vector<std::string>{"summary"}},
            {"additionalProperties", false}
        };
    }
    const Spec& spec() const override { return spec_; }
    std::expected<std::unique_ptr<Call>, Result> prepare(std::string_view arguments,
                                                         Context&) const override {
        auto args = detail::parse_arguments(arguments);
        if (!args) return std::unexpected(detail::error_result(args.error()));
        std::string error;
        std::string summary = detail::require_string(*args, "summary", error);
        if (!error.empty()) return std::unexpected(detail::error_result(error));
        agent::AskView view;
        view.header = "Plan ready";
        view.prompt = summary;
        view.allow_other = false;
        view.options = {{"Accept and start", "Switch to workspace mode and implement now"},
                        {"Accept with confirmations", "Switch to ask mode and implement with approvals"},
                        {"Continue planning", "Stay read-only and refine the plan"}};
        return std::make_unique<InteractiveCall>(agent::ToolKind::exit_plan, std::move(view), std::move(summary));
    }
private:
    Spec spec_;
};

} // namespace

std::unique_ptr<Tool> detail::make_ask_tool() { return std::make_unique<AskTool>(); }
std::unique_ptr<Tool> detail::make_exit_plan_tool() { return std::make_unique<ExitPlanTool>(); }

} // namespace dagent::tools
