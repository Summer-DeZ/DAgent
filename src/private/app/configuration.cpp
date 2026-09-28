#include "app/configuration.hpp"
#include "app/config.hpp"

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
    std::filesystem::path root, std::vector<std::string> overrides,
    std::map<std::string, llm::ProviderConfig> models, std::filesystem::path theme_file,
    std::function<std::shared_ptr<agent::ModelSession>(const llm::ProviderConfig&)> make_session)
    : root_(std::move(root)), theme_file_(std::move(theme_file)), overrides_(std::move(overrides)),
      models_(std::move(models)), make_session_(std::move(make_session)) {}

std::vector<agent::PublicModel> Configuration::models() const {
    std::vector<agent::PublicModel> out;
    out.reserve(models_.size());
    for (const auto& [name, model] : models_) out.push_back(llm::to_public(model));
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
    models_[saved.name] = saved;
    return llm::to_public(saved);
}

std::filesystem::path Configuration::theme_file() const { return theme_file_; }

ModelSelection Configuration::resolve(const std::string& name) {
    auto overrides = overrides_;
    overrides.push_back("@model=" + name);
    ModelCatalog catalog = load_models(root_, overrides);
    for (const auto& note : catalog.selection_log) base::logger("app")->info("{}", note);
    const auto it = catalog.models.find(name);
    if (it == catalog.models.end()) throw std::runtime_error("unknown model: " + name);
    return ModelSelection{llm::to_public(it->second), make_session_(it->second)};
}

} // namespace dagent::app
