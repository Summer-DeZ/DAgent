#include "agent/subagent.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <mutex>
#include <utility>

#include "agent/host.hpp"
#include "base/log.hpp"
#include "base/text.hpp"

namespace dagent::agent {
namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

std::string first_characters(std::string_view text, std::size_t count) {
    std::size_t pos = 0;
    for (std::size_t seen = 0; pos < text.size() && seen < count; ++seen) {
        const auto lead = static_cast<unsigned char>(text[pos]);
        pos += lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    }
    return std::string(text.substr(0, std::min(pos, text.size())));
}

tools::Result error_result(std::string text) {
    tools::Result result;
    result.text = std::move(text);
    result.is_error = true;
    return result;
}

class TaskTool;

class TaskCall final : public tools::Call {
public:
    TaskCall(const TaskTool& owner, std::string agent, std::string prompt);

    const std::string& agent() const { return agent_; }
    const std::string& prompt() const { return prompt_; }

private:
    tools::Result do_run(const tools::Grant&, const std::function<void(std::string_view)>&,
                         std::stop_token) override;

    const TaskTool& owner_;
    std::string agent_, prompt_;
};

class TaskTool final : public tools::Tool {
public:
    explicit TaskTool(Agent& owner) : owner_(owner), parent_tools_(owner.tool_names()) {
        const std::vector<SubagentDef>& defs = owner.setup().subagents;
        spec_.name = "task";
        std::string description =
            "Delegate a self-contained task to a subagent that runs in its own context.\n"
            "The subagent cannot see this conversation, so the prompt must be self-sufficient.\n"
            "It cannot ask the user questions and cannot delegate further.\n"
            "Subagents dispatched in the same message run concurrently - do not have two of them edit the same files.\n"
            "Available agents:";
        nlohmann::json names = nlohmann::json::array();
        for (const SubagentDef& def : defs) {
            description += "\n- " + def.name + ": " + def.description;
            names.push_back(def.name);
        }
        spec_.description = std::move(description);
        spec_.parameters = {
            {"type", "object"},
            {"properties",
             {{"agent", {{"type", "string"}, {"enum", std::move(names)}}},
              {"prompt",
               {{"type", "string"}, {"description", "The complete, self-contained task"}}}}},
            {"required", {"agent", "prompt"}},
            {"additionalProperties", false},
        };
    }

    const tools::Spec& spec() const override { return spec_; }

    std::expected<std::unique_ptr<tools::Call>, tools::Result> prepare(std::string_view arguments,
                                                                       tools::Context&) const override {
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(arguments);
        } catch (const nlohmann::json::exception&) {
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
        const auto known = std::ranges::find_if(owner_.setup().subagents, [&](const SubagentDef& def) {
            return def.name == name;
        });
        if (known == owner_.setup().subagents.end())
            return std::unexpected(error_result(std::format("unknown subagent: {}", name)));
        return std::make_unique<TaskCall>(*this, name, prompt->get<std::string>());
    }

    Agent& owner() const { return owner_; }
    const std::vector<std::string>& parent_tools() const { return parent_tools_; }

private:
    tools::Spec spec_;
    Agent& owner_;
    std::vector<std::string> parent_tools_;
};

TaskCall::TaskCall(const TaskTool& owner, std::string agent, std::string prompt)
    : owner_(owner), agent_(std::move(agent)), prompt_(std::move(prompt)) {
    intent_.kind = tools::Intent::Kind::task;
    intent_.summary = "task(" + agent_ + "): " + first_characters(prompt_, 60);
}

tools::Result TaskCall::do_run(const tools::Grant&, const std::function<void(std::string_view)>&,
                               std::stop_token stop) {
    if (owner_.owner().setup().subagent_depth >= 1) // 三重保险之三
        return error_result("task is not available inside a subagent");
    const TurnContext* parent = owner_.owner().current_turn();
    if (parent == nullptr) return error_result("task cannot run outside a turn");
    const AgentHost& host = *owner_.owner().setup().host;
    const SubagentDef* def = host.find_subagent(agent_);
    if (def == nullptr) return error_result(std::format("unknown subagent: {}", agent_));

    const DerivedPermission derived =
        derive_permission(owner_.owner().permission_mode(), owner_.owner().planning(),
                          owner_.owner().read_only(), def->permission);
    Setup child_setup = derive_child_setup(owner_.owner().setup(), *def, derived,
                                           owner_.owner().meta().id, owner_.parent_tools());

    tools::TaskView view;
    view.agent = def->name;
    view.task = prompt_;

    std::unique_ptr<Agent> child;
    try {
        child = Agent::create_child(std::move(child_setup));
    } catch (const std::exception& error) {
        return error_result(std::format("failed to start subagent {}: {}", def->name, error.what()));
    }
    view.session_id = child->meta().id;
    const std::string child_session = view.session_id;
    const std::string parent_call_id = call_id();

    std::mutex view_mutex;
    std::string current_text, failure;
    const auto child_sink = [&](const Event& event) {
        {
            const std::lock_guard lock(view_mutex);
            std::visit(Overloaded{
                           [&](const StepStarted&) { current_text.clear(); },
                           [&](const TextDelta& delta) { current_text += delta.text; },
                           [&](const StreamReset&) { current_text.clear(); },
                           [&](const ToolFinished& finished) {
                               view.steps.push_back({finished.summary, finished.result.is_error});
                           },
                           [&](const TurnEnded& ended) {
                               view.model_calls = ended.steps;
                               view.tool_calls = ended.tool_calls;
                               if (ended.status == TurnStatus::interrupted) view.interrupted = true;
                               if (!current_text.empty()) view.result = current_text;
                               if (ended.status == TurnStatus::failed && !ended.error.empty())
                                   failure = ended.error;
                           },
                           [&](const auto&) {},
                       },
                       event);
        }
        parent->sink(SubEvent{child_session, def->name, parent_call_id,
                              std::make_shared<const EventBox>(EventBox{event})});
    };
    const Approver child_approver = derived.may_ask
        ? Approver{[&](const Approval& approval, std::stop_token token) {
              Approval copy = approval;
              copy.agent = def->name;
              copy.origin_call_id = parent_call_id;
              return owner_.owner().setup().host->approve(copy, parent->approver, token);
          }}
        : Approver{};
    const Asker empty_asker{};

    const auto began = std::chrono::steady_clock::now();
    child->run_turn(prompt_, TurnContext{child_sink, child_approver, empty_asker, stop});
    view.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

    {
        const std::lock_guard lock(view_mutex);
        if (view.result.empty()) view.result = failure;
    }
    view.interrupted = view.interrupted || stop.stop_requested();

    tools::Result result;
    result.interrupted = view.interrupted;
    if (view.result.empty()) {
        result.text = std::format("Subagent {} produced no conclusion", def->name);
        if (!failure.empty()) result.text += ": " + failure;
        result.is_error = true;
    } else {
        result.text = view.result;
        result.is_error = !failure.empty();
    }
    result.display = std::move(view);
    return result;
}

} // namespace

Setup derive_child_setup(const Setup& parent, const SubagentDef& def, const DerivedPermission& permission,
                         std::string_view parent_session_id,
                         const std::vector<std::string>& parent_tool_names) {
    Setup child = parent;
    child.provider = def.model.empty() ? parent.provider : parent.host->models().at(def.model);
    child.options.run.max_model_calls =
        def.max_model_calls != 0 ? def.max_model_calls : parent.options.run.max_model_calls;
    child.options.run.max_tool_calls =
        def.max_tool_calls != 0 ? def.max_tool_calls : parent.options.run.max_tool_calls;
    child.permission_mode = permission.mode;
    child.read_only = permission.read_only;
    child.planning = permission.planning;
    child.system_prompt = def.system_prompt;
    child.subagent_depth = parent.subagent_depth + 1;
    child.parent_session_id = std::string(parent_session_id);
    child.subagent_name = def.name;
    child.subagents.clear();
    child.mcp_servers.clear();
    child.host = parent.host;

    std::vector<std::string> allowed = def.tools.empty() ? parent_tool_names : def.tools;
    std::erase_if(allowed, [&](const std::string& name) {
        if (name == "task" || name == "ask" || name == "exit_plan") return true;
        return def.tools.empty() && name.starts_with("mcp__"); // 默认不含 MCP 工具
    });
    child.allowed_tools = std::move(allowed);
    return child;
}

std::unique_ptr<tools::Tool> make_task_tool(Agent& owner) {
    if (owner.setup().subagent_depth > 0 || owner.setup().subagents.empty()) return nullptr;
    return std::make_unique<TaskTool>(owner);
}

} // namespace dagent::agent
