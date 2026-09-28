#include "agent/control.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include "agent/port_delegation.hpp"
#include "base/json.hpp"
#include "lib/nlohmann/json.hpp"

namespace dagent::agent {
namespace {

using nlohmann::json;

using base::parse_arguments;
using base::require_string;
using base::get_bool;

ToolResult error_result(std::string text) {
    ToolResult result;
    result.model_text = std::move(text);
    result.is_error = true;
    return result;
}

/// task 摘要里的 prompt 前缀，按 UTF-8 字符边界截断。
std::string first_characters(std::string_view text, std::size_t count) {
    std::size_t pos = 0;
    for (std::size_t seen = 0; pos < text.size() && seen < count; ++seen) {
        const auto lead = static_cast<unsigned char>(text[pos]);
        pos += lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    }
    return std::string(text.substr(0, std::min(pos, text.size())));
}

Question to_question(std::string_view call_id, const AskView& view, bool allow_other) {
    Question question;
    question.call_id = call_id;
    question.header = view.header;
    question.prompt = view.prompt;
    question.multi_select = view.multi_select;
    question.allow_other = allow_other;
    for (const auto& option : view.options)
        question.options.push_back({option.label, option.description});
    return question;
}

std::expected<ControlRequest, ToolResult> parse_ask(std::string_view arguments,
                                                    std::string_view call_id) {
    auto args = parse_arguments(arguments);
    if (!args) return std::unexpected(error_result(args.error()));
    std::string error;
    AskView view;
    view.header = require_string(*args, "header", error);
    view.prompt = require_string(*args, "prompt", error);
    const auto multi = get_bool(*args, "multi_select", error);
    const auto other = get_bool(*args, "allow_other", error);
    if (!error.empty()) return std::unexpected(error_result(error));
    view.multi_select = multi.value_or(false);
    view.allow_other = other.value_or(true);

    const auto options = args->find("options");
    if (options == args->end() || !options->is_array() || options->size() < 2 || options->size() > 4)
        return std::unexpected(error_result("options must contain 2 to 4 entries"));
    for (const auto& entry : *options) {
        if (!entry.is_object()) return std::unexpected(error_result("each option must be an object"));
        const auto label = entry.find("label");
        const auto description = entry.find("description");
        if (label == entry.end() || !label->is_string() || label->get_ref<const std::string&>().empty() ||
            description == entry.end() || !description->is_string()) {
            return std::unexpected(error_result(
                "each option needs a non-empty label and string description"));
        }
        view.options.push_back({label->get<std::string>(), description->get<std::string>()});
    }

    AskRequest request;
    request.call_id = call_id;
    request.summary = "Ask the user: " + view.prompt;
    request.view = std::move(view);
    return ControlRequest{std::move(request)};
}

std::expected<ControlRequest, ToolResult> parse_exit_plan(std::string_view arguments,
                                                          std::string_view call_id) {
    auto args = parse_arguments(arguments);
    if (!args) return std::unexpected(error_result(args.error()));
    std::string error;
    std::string summary = require_string(*args, "summary", error);
    if (!error.empty()) return std::unexpected(error_result(error));

    PlanConfirmation request;
    request.call_id = call_id;
    request.summary = "Submit implementation plan";
    request.plan = std::move(summary);
    request.view.header = "Plan ready";
    request.view.prompt = request.plan;
    request.view.allow_other = false;
    request.view.options = {{"Accept and start", "Switch to workspace mode and implement now"},
                            {"Accept with confirmations", "Switch to ask mode and implement with approvals"},
                            {"Continue planning", "Stay read-only and refine the plan"}};
    return ControlRequest{std::move(request)};
}

std::expected<ControlRequest, ToolResult> parse_todo(std::string_view arguments) {
    auto args = parse_arguments(arguments);
    if (!args) return std::unexpected(error_result(args.error()));
    const auto it = args->find("items");
    if (it == args->end() || !it->is_array())
        return std::unexpected(error_result("items must be an array"));

    TodoView view;
    for (const auto& raw : *it) {
        if (!raw.is_object())
            return std::unexpected(error_result("each item in items must be an object"));
        const auto text = raw.find("text");
        const auto state = raw.find("state");
        if (text == raw.end())
            return std::unexpected(error_result("plan item text is required"));
        if (!text->is_string() || text->get<std::string>().empty())
            return std::unexpected(error_result("plan item text must be a non-empty string"));
        if (state == raw.end())
            return std::unexpected(error_result("plan item state is required"));
        if (!state->is_string())
            return std::unexpected(error_result("plan item state must be a string"));

        TodoItem item;
        item.text = text->get<std::string>();
        const std::string value = state->get<std::string>();
        if (value == "todo") item.state = TodoItem::State::todo;
        else if (value == "doing") item.state = TodoItem::State::doing;
        else if (value == "done") item.state = TodoItem::State::done;
        else if (value == "dropped") item.state = TodoItem::State::dropped;
        else return std::unexpected(error_result(
            "plan item state must be todo, doing, done or dropped"));
        view.items.push_back(std::move(item));
    }

    PlanReplacement request;
    request.summary = std::format("Update {} plan items", view.items.size());
    request.plan = std::move(view);
    return ControlRequest{std::move(request)};
}

std::expected<ControlRequest, ToolResult> parse_task(std::string_view arguments,
                                                     std::string_view call_id,
                                                     const std::vector<SubagentDef>& subagents) {
    json parsed;
    try {
        parsed = json::parse(arguments);
    } catch (const json::exception&) {
        return std::unexpected(error_result("task arguments must be a JSON object"));
    }
    if (!parsed.is_object()) return std::unexpected(error_result("task arguments must be a JSON object"));
    const auto agent = parsed.find("agent");
    const auto prompt = parsed.find("prompt");
    if (agent == parsed.end() || !agent->is_string() || agent->get<std::string>().empty())
        return std::unexpected(error_result("task requires an agent name"));
    if (prompt == parsed.end() || !prompt->is_string() || prompt->get<std::string>().empty())
        return std::unexpected(error_result("task requires a non-empty prompt"));
    const std::string name = agent->get<std::string>();
    const auto known = std::ranges::find_if(subagents, [&](const SubagentDef& def) {
        return def.name == name;
    });
    if (known == subagents.end())
        return std::unexpected(error_result(std::format("unknown subagent: {}", name)));

    DelegationRequest request;
    request.call_id = call_id;
    request.agent = name;
    request.prompt = prompt->get<std::string>();
    request.summary = "task(" + request.agent + "): " + first_characters(request.prompt, 60);
    return ControlRequest{std::move(request)};
}

ToolSpec todo_spec() {
    ToolSpec spec;
    spec.name = "todo";
    spec.description =
        "Maintain the plan for the current task. For multi-step work, call this tool before any other tool, even if the user already listed the steps. Call it again immediately when starting, completing or dropping an item. Do not replace these calls with a prose checklist. "
        "Submit the entire list on every call, not an incremental update.";
    spec.parameters = {
        {"type", "object"},
        {"properties", {{"items", {
            {"type", "array"},
            {"description", "Complete current plan in execution order"},
            {"items", {
                {"type", "object"},
                {"properties", {
                    {"text", {{"type", "string"}, {"description", "A short, actionable plan item"}}},
                    {"state", {{"type", "string"},
                               {"enum", std::vector<std::string>{"todo", "doing", "done", "dropped"}}}}
                }},
                {"required", std::vector<std::string>{"text", "state"}},
                {"additionalProperties", false}
            }}
        }}}},
        {"required", std::vector<std::string>{"items"}},
        {"additionalProperties", false}
    };
    return spec;
}

ToolSpec ask_spec() {
    ToolSpec spec;
    spec.name = "ask";
    spec.description =
        "Ask the user to choose only when different choices would materially change the work. "
        "Do not ask for facts you can discover yourself. Provide 2 to 4 concise options.";
    spec.parameters = {
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
    return spec;
}

ToolSpec exit_plan_spec() {
    ToolSpec spec;
    spec.name = "exit_plan";
    spec.description = "Submit a complete implementation plan for user approval. Use only in planning mode.";
    spec.parameters = {
        {"type", "object"},
        {"properties", {{"summary", {{"type", "string"}, {"description", "Complete proposed plan"}}}}},
        {"required", std::vector<std::string>{"summary"}},
        {"additionalProperties", false}
    };
    return spec;
}

ToolSpec task_spec(const std::vector<SubagentDef>& subagents) {
    ToolSpec spec;
    spec.name = "task";
    std::string description =
        "Delegate a self-contained task to a subagent that runs in its own context.\n"
        "The subagent cannot see this conversation, so the prompt must be self-sufficient.\n"
        "It cannot ask the user questions and cannot delegate further.\n"
        "Subagents dispatched in the same message run concurrently - do not have two of them edit the same files.\n"
        "Available agents:";
    json names = json::array();
    for (const SubagentDef& def : subagents) {
        description += "\n- " + def.name + ": " + def.description;
        names.push_back(def.name);
    }
    spec.description = std::move(description);
    spec.parameters = {
        {"type", "object"},
        {"properties",
         {{"agent", {{"type", "string"}, {"enum", std::move(names)}}},
          {"prompt",
           {{"type", "string"}, {"description", "The complete, self-contained task"}}}}},
        {"required", {"agent", "prompt"}},
        {"additionalProperties", false},
    };
    return spec;
}

} // namespace

std::vector<ToolSpec> control_action_specs(bool include_task, const std::vector<SubagentDef>& subagents) {
    std::vector<ToolSpec> specs;
    specs.push_back(todo_spec());
    specs.push_back(ask_spec());
    specs.push_back(exit_plan_spec());
    if (include_task && !subagents.empty()) specs.push_back(task_spec(subagents));
    return specs;
}

std::expected<ControlRequest, ToolResult> parse_control_action(std::string_view name,
                                                               std::string_view arguments,
                                                               std::string_view call_id,
                                                               const std::vector<SubagentDef>& subagents) {
    if (name == "skill") {
        auto args = parse_arguments(arguments);
        if (!args) return std::unexpected(error_result(args.error()));
        std::string error;
        auto selected = require_string(*args, "name", error);
        if (!error.empty()) return std::unexpected(error_result(error));
        return ControlRequest{SkillActivation{"Load skill " + selected, std::move(selected)}};
    }
    if (name == "todo") return parse_todo(arguments);
    if (name == "ask") return parse_ask(arguments, call_id);
    if (name == "exit_plan") return parse_exit_plan(arguments, call_id);
    if (name == "task") return parse_task(arguments, call_id, subagents);
    return std::unexpected(error_result(std::format("unknown control action: {}", name)));
}

void ControlActionExecutor::begin_turn(Services services) {
    services_ = std::move(services);
    questions_this_turn_ = 0;
}

ToolResult ControlActionExecutor::execute(const ControlRequest& request, std::stop_token stop) {
    if (const auto* ask = std::get_if<AskRequest>(&request)) return run_ask(*ask, stop);
    if (const auto* plan = std::get_if<PlanConfirmation>(&request)) return run_plan_confirmation(*plan, stop);
    if (const auto* replacement = std::get_if<PlanReplacement>(&request)) return run_plan_replacement(*replacement);
    if (const auto* skill = std::get_if<SkillActivation>(&request)) {
        if (stop.stop_requested()) { ToolResult result; result.interrupted = true; return result; }
        return services_.activate_skill(skill->name);
    }
    return run_delegation(std::get<DelegationRequest>(request), stop);
}

ToolResult ControlActionExecutor::run_ask(const AskRequest& request, std::stop_token stop) {
    ToolResult result;
    if (++questions_this_turn_ > 3) {
        result.model_text =
            "Question limit reached for this turn. Make the most reasonable choice, state the assumption, and continue.";
        result.is_error = true;
        result.display = request.view;
        return result;
    }
    if (services_.asker == nullptr || !*services_.asker) {
        result.model_text =
            "Non-interactive run: cannot ask the user. Pick the most reasonable option, state the assumption you made, and continue.";
        result.is_error = true;
        result.display = request.view;
        return result;
    }

    const Answer answer = (*services_.asker)(to_question(request.call_id, request.view, request.view.allow_other), stop);
    AskView view = request.view;
    view.selected = answer.selected;
    view.other = answer.other;
    view.cancelled = answer.cancelled;
    if (answer.cancelled || stop.stop_requested()) {
        result.model_text = "The user cancelled the question.";
        result.interrupted = true;
        result.display = std::move(view);
        return result;
    }
    std::string choices;
    for (const int index : answer.selected) {
        if (index < 0 || static_cast<std::size_t>(index) >= view.options.size()) continue;
        if (!choices.empty()) choices += ", ";
        choices += view.options[static_cast<std::size_t>(index)].label;
    }
    if (!answer.other.empty()) {
        if (!choices.empty()) choices += ", ";
        choices += answer.other;
    }
    result.model_text = "User chose: " + choices;
    result.display = std::move(view);
    return result;
}

ControlActionExecutor::PlanDecision ControlActionExecutor::plan_decision(const Answer& answer) const {
    if (answer.selected.empty() || answer.selected.front() == 2) return PlanDecision::continue_planning;
    return answer.selected.front() == 0 ? PlanDecision::accept_workspace : PlanDecision::accept_ask;
}

ToolResult ControlActionExecutor::run_plan_confirmation(const PlanConfirmation& request,
                                                       std::stop_token stop) {
    ToolResult result;
    if (services_.policy == nullptr || !services_.policy->planning()) {
        result.model_text = "exit_plan is only available while planning.";
        result.is_error = true;
        return result;
    }
    if (services_.asker == nullptr || !*services_.asker) {
        result.model_text =
            "Non-interactive planning run: the plan cannot be confirmed. Present the final plan and stop without making changes.";
        result.is_error = true;
        result.display = request.view;
        return result;
    }

    const Answer answer =
        (*services_.asker)(to_question(request.call_id, request.view, false), stop);
    AskView view = request.view;
    view.selected = answer.selected;
    view.cancelled = answer.cancelled;
    if (answer.cancelled || stop.stop_requested()) {
        result.model_text = "Plan confirmation was cancelled.";
        result.interrupted = true;
        result.display = std::move(view);
        return result;
    }

    switch (plan_decision(answer)) {
    case PlanDecision::accept_workspace:
    case PlanDecision::accept_ask: {
        const PermissionMode next = plan_decision(answer) == PlanDecision::accept_workspace
                                        ? PermissionMode::workspace
                                        : PermissionMode::ask;
        services_.policy->set_mode(next);
        services_.policy->set_planning(false);
        services_.policy->set_read_only(services_.base_read_only);
        services_.sink(ModeChanged{std::string(to_string(next)), false});
        result.model_text = "Plan accepted. Begin implementation now.";
        break;
    }
    case PlanDecision::continue_planning:
        result.model_text = "Continue planning. Refine the proposal and submit it again when ready.";
        break;
    }
    result.display = std::move(view);
    return result;
}

ToolResult ControlActionExecutor::run_plan_replacement(const PlanReplacement& request) {
    const auto done = std::ranges::count_if(request.plan.items, [](const TodoItem& item) {
        return item.state == TodoItem::State::done;
    });
    ToolResult result;
    result.model_text = std::format("Plan updated: {}/{} done.", done, request.plan.items.size());
    result.display = request.plan;
    return result;
}

ToolResult ControlActionExecutor::run_delegation(const DelegationRequest& request, std::stop_token stop) {
    if (services_.delegation == nullptr) return error_result("task is not available in this run");
    DelegationContext context;
    context.parent_session_id = services_.session_id;
    context.call_id = request.call_id;
    if (services_.policy != nullptr) {
        context.parent_mode = services_.policy->mode();
        context.parent_planning = services_.policy->planning();
        context.parent_read_only = services_.policy->read_only();
    }
    context.model = services_.model;
    context.parent_tools = services_.tool_names;
    context.sink = &services_.sink;
    context.approver = services_.approver;
    context.stop = stop;
    return services_.delegation->delegate(context, request);
}

} // namespace dagent::agent
