#include "app/prompt.hpp"

namespace dagent::app {

std::string render_system_prompt(std::string_view tmpl, const workspace::Environment& env,
                                 const PromptVars& vars) {
    nlohmann::json data = workspace::to_json(env);
    data["model"] = vars.model;
    data["project_root"] = vars.project_root.string();
    data["sandbox"] = vars.sandbox;
    data["workspace_sandbox"] = vars.workspace_sandbox;
    data["sandbox_backend"] = vars.sandbox_backend;
    data["sandbox_missing"] = vars.sandbox_missing;
    data["sandbox_child_signals"] = vars.sandbox_child_signals;
    data["permission_mode"] = vars.permission_mode;
    return workspace::render(tmpl, data);
}

} // namespace dagent::app
