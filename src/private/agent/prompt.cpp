#include "agent/prompt.hpp"

namespace dagent::agent {

std::string render_system_prompt(std::string_view tmpl, const workspace::Environment& env,
                                 const PromptVars& vars) {
    nlohmann::json data = workspace::to_json(env);
    data["model"] = vars.model;
    data["project_root"] = vars.project_root.string();
    data["sandbox"] = vars.sandbox;
    data["permission_mode"] = vars.permission_mode;
    return workspace::render(tmpl, data);
}

} // namespace dagent::agent
