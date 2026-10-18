#include "app/decision_profile.hpp"

#include <fstream>
#include <initializer_list>
#include <string_view>

#include "app/config.hpp"
#include "base/json.hpp"
#include "storage/decision_store.hpp"

namespace dagent::app {
namespace {
using namespace dagent::agent;
using nlohmann::json;

[[noreturn]] void invalid(const std::string& message) {
    throw ConfigError(ConfigError::Kind::invalid, "decision profile: " + message);
}

void fields(const json& object, std::initializer_list<std::string_view> allowed) {
    if (!object.is_object()) invalid("expected an object");
    for (const auto& [name, value] : object.items()) {
        bool found = false;
        for (auto field : allowed) if (name == field) found = true;
        if (!found) invalid("unknown field " + name);
    }
}

ReviewScene scene_from(std::string_view name) {
    for (auto scene : {ReviewScene::command_network, ReviewScene::script_basic,
                       ReviewScene::script_mixed_basic, ReviewScene::script_diff,
                       ReviewScene::outside_read, ReviewScene::mcp_action, ReviewScene::fetch_url})
        if (to_string(scene) == name) return scene;
    invalid("unknown action scene " + std::string(name));
}

AdaptiveThreshold threshold(const json& value, AdaptiveThreshold::Pass pass) {
    AdaptiveThreshold result;
    result.pass = pass;
    if (value.is_number()) {
        result.strict_value = result.relaxed_value = result.current_value = value.get<double>();
    } else {
        fields(value, {"strict", "relaxed", "current"});
        result.strict_value = value.at("strict").get<double>();
        result.relaxed_value = value.at("relaxed").get<double>();
        result.current_value = value.at("current").get<double>();
    }
    return result;
}
} // namespace

agent::ThresholdProfile load_decision_profile(const std::filesystem::path& path,
                                              const agent::ReviewVersion& expected) {
    std::ifstream input(path);
    if (!input) throw ConfigError(ConfigError::Kind::io, "cannot open decision profile: " + path.string());
    try {
        const json document = json::parse(input, nullptr, true, true);
        fields(document, {"version", "scenes", "injection_limit"});
        const auto& version = document.at("version");
        fields(version, {"provider_kind", "adapter_version", "endpoint", "model", "model_revision",
                         "question_version", "threshold_profile"});
        ThresholdProfile profile;
        profile.version = {version.at("provider_kind").get<std::string>(),
                           version.at("adapter_version").get<std::string>(),
                           version.at("endpoint").get<std::string>(),
                           version.at("model").get<std::string>(),
                           version.at("model_revision").get<std::string>(),
                           version.at("question_version").get<std::string>(),
                           version.at("threshold_profile").get<std::string>()};
        if (profile.version != expected)
            invalid("version does not match the selected model, questions and threshold profile");
        const auto scenes = document.find("scenes");
        if (scenes != document.end()) {
            if (!scenes->is_object()) invalid("scenes must be an object");
            for (const auto& [name, values] : scenes->items()) {
                const auto scene = scene_from(name);
                fields(values, {"risk_low", "risk_high", "unknown_policy", "evidence_sufficient",
                                "manual_only", "authorization", "privacy", "direction"});
                auto& risk = profile.risk[scene];
                risk.low = values.at("risk_low").get<double>();
                risk.high = values.at("risk_high").get<double>();
                const auto policy = values.at("unknown_policy").get<std::string>();
                if (policy != "authorize" && policy != "defer") invalid(name + ": invalid unknown_policy");
                risk.unknown_policy = policy == "authorize" ? RiskThresholds::UnknownPolicy::authorize
                                                            : RiskThresholds::UnknownPolicy::defer;
                for (auto dimension : {Dimension::evidence_sufficient, Dimension::manual_only,
                                       Dimension::authorization, Dimension::privacy}) {
                    const auto entry = values.find(std::string(to_string(dimension)));
                    if (entry == values.end()) continue;
                    const auto pass = dimension == Dimension::manual_only || dimension == Dimension::privacy
                        ? AdaptiveThreshold::Pass::at_most : AdaptiveThreshold::Pass::at_least;
                    profile.adaptive.emplace(ThresholdKey{scene, dimension, {}}, threshold(*entry, pass));
                }
                if (const auto directions = values.find("direction"); directions != values.end()) {
                    fields(*directions, {"fetch", "send"});
                    for (auto direction : {Direction::fetch, Direction::send}) {
                        const auto entry = directions->find(std::string(to_string(direction)));
                        if (entry != directions->end())
                            profile.adaptive.emplace(ThresholdKey{scene, Dimension::direction, direction},
                                threshold(*entry, AdaptiveThreshold::Pass::at_least));
                    }
                }
            }
        }
        if (document.contains("injection_limit"))
            profile.injection_limit = document.at("injection_limit").get<double>();
        storage::validate_decision_profile(profile);
        return profile;
    } catch (const json::exception& error) {
        throw ConfigError(ConfigError::Kind::parse, "decision profile " + path.string() + ": " + error.what());
    } catch (const storage::StorageError& error) {
        invalid(error.what());
    }
}

} // namespace dagent::app
