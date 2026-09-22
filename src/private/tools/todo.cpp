#include <format>
#include <utility>

#include "tools/detail.hpp"

namespace dagent::tools {
namespace {

class TodoCall final : public Call {
public:
    explicit TodoCall(agent::TodoView view) : view_(std::move(view)) {
        intent_.kind = agent::ToolKind::read;
        intent_.summary = std::format("Update {} plan items", view_.items.size());
    }

private:
    Result do_run(const Grant&, const std::function<void(std::string_view)>&,
                  std::stop_token) override {
        const auto done = std::ranges::count_if(view_.items, [](const agent::TodoItem& item) {
            return item.state == agent::TodoItem::State::done;
        });
        Result result;
        result.model_text = std::format("Plan updated: {}/{} done.", done, view_.items.size());
        result.display = view_;
        return result;
    }

    agent::TodoView view_;
};

class TodoTool final : public Tool {
public:
    TodoTool() {
        spec_.name = "todo";
        spec_.description =
            "Maintain the plan for the current task. For multi-step work, call this tool before any other tool, even if the user already listed the steps. Call it again immediately when starting, completing or dropping an item. Do not replace these calls with a prose checklist. "
            "Submit the entire list on every call, not an incremental update.";
        spec_.parameters = {
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
    }

    const Spec& spec() const override { return spec_; }

    std::expected<std::unique_ptr<Call>, Result> prepare(std::string_view arguments,
                                                         Context&) const override {
        auto args = detail::parse_arguments(arguments);
        if (!args) return std::unexpected(detail::error_result(args.error()));
        const auto it = args->find("items");
        if (it == args->end() || !it->is_array())
            return std::unexpected(detail::error_result("items must be an array"));

        agent::TodoView view;
        for (const auto& raw : *it) {
            if (!raw.is_object())
                return std::unexpected(detail::error_result("each item in items must be an object"));
            const auto text = raw.find("text");
            const auto state = raw.find("state");
            if (text == raw.end())
                return std::unexpected(detail::error_result("plan item text is required"));
            if (!text->is_string() || text->get<std::string>().empty())
                return std::unexpected(detail::error_result("plan item text must be a non-empty string"));
            if (state == raw.end())
                return std::unexpected(detail::error_result("plan item state is required"));
            if (!state->is_string())
                return std::unexpected(detail::error_result("plan item state must be a string"));

            agent::TodoItem item;
            item.text = text->get<std::string>();
            const std::string value = state->get<std::string>();
            if (value == "todo") item.state = agent::TodoItem::State::todo;
            else if (value == "doing") item.state = agent::TodoItem::State::doing;
            else if (value == "done") item.state = agent::TodoItem::State::done;
            else if (value == "dropped") item.state = agent::TodoItem::State::dropped;
            else return std::unexpected(detail::error_result(
                "plan item state must be todo, doing, done or dropped"));
            view.items.push_back(std::move(item));
        }
        return std::make_unique<TodoCall>(std::move(view));
    }

private:
    Spec spec_;
};

} // namespace

std::unique_ptr<Tool> detail::make_todo_tool() { return std::make_unique<TodoTool>(); }

} // namespace dagent::tools
