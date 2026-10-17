#include "llm/decision_provider.hpp"

#include <algorithm>
#include <stdexcept>

#include "llm/decision_systemone.hpp"

namespace dagent::llm {

const DecisionProviderInfo* find_decision_provider(std::string_view kind) {
    static const std::vector<DecisionProviderInfo> providers = decision_providers();
    const auto found = std::ranges::find(providers, kind, &DecisionProviderInfo::kind);
    return found == providers.end() ? nullptr : &*found;
}

std::shared_ptr<agent::DecisionModel> make_decision_model(const DecisionProviderConfig& config,
                                                          std::chrono::milliseconds timeout,
                                                          net::HttpOptions http) {
    const DecisionProviderInfo* info = find_decision_provider(config.kind);
    if (info == nullptr) throw std::invalid_argument("unsupported decision provider kind: " + config.kind);
    if (config.endpoint.empty())
        throw std::invalid_argument("decision model " + config.name + " has an empty endpoint");
    if (!info->capabilities.boolean_probability || !info->capabilities.choice_probabilities)
        throw std::invalid_argument("decision provider " + config.kind + " does not provide the required probabilities");
    if (config.kind == "systemone") return make_systemone_decision_model(config, timeout, std::move(http));
    throw std::invalid_argument("unsupported decision provider kind: " + config.kind);
}

} // namespace dagent::llm
