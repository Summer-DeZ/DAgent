#include "app/configuration.hpp"

#include <format>
#include <stdexcept>
#include <utility>

#include "base/log.hpp"
#include "llm/llm.hpp"

namespace dagent::app {
namespace {

llm::ProviderConfig to_internal(const agent::ModelInput& input) {
    llm::ProviderConfig model;
    model.kind = input.kind;
    model.name = input.name;
    model.base_url = input.base_url;
    model.model = input.model;
    model.api_key = input.credential;
    model.max_tokens = input.max_tokens;
    model.context_window = input.context_window;
    return model;
}

} // namespace

Configuration::Configuration(
    std::filesystem::path root, std::filesystem::path cwd, std::vector<std::string> overrides,
    Config config, std::function<std::shared_ptr<agent::ModelSession>(const llm::ProviderConfig&)> make_session)
    : root_(std::move(root)), cwd_(std::move(cwd)), overrides_(std::move(overrides)),
      config_(std::move(config)), make_session_(std::move(make_session)) {}

std::vector<agent::PublicModel> Configuration::models() const {
    std::vector<agent::PublicModel> out;
    out.reserve(config_.models.size());
    for (const auto& [name, model] : config_.models) out.push_back(llm::to_public(model));
    return out;
}

std::vector<agent::ProviderKindInfo> Configuration::provider_kinds() const {
    std::vector<agent::ProviderKindInfo> out;
    for (const llm::ProviderInfo& info : llm::providers()) {
        out.push_back({std::string(info.kind), std::string(info.default_base_url), info.needs_api_key});
    }
    return out;
}

agent::PublicModel Configuration::add_model(const agent::ModelInput& input) {
    const llm::ProviderConfig saved = app::add_model(root_, to_internal(input));
    config_.models[saved.name] = saved;
    return llm::to_public(saved);
}

std::filesystem::path Configuration::theme_file() const { return config_.ui.theme_file; }

ModelSelection Configuration::resolve(const std::string& name) {
    auto overrides = overrides_;
    overrides.push_back("@model=" + name);
    Config config = load_config({root_, cwd_, overrides});
    for (const auto& note : config.model_selection_log) base::logger("app")->info("{}", note);
    const auto it = config.models.find(name);
    if (it == config.models.end()) throw std::runtime_error("unknown model: " + name);
    return ModelSelection{llm::to_public(it->second), make_session_(it->second)};
}

} // namespace dagent::app
