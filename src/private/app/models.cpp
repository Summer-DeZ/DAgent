#include "app/config_detail.hpp"
#include "app/home.hpp"
#include "llm/llm.hpp"

#include <format>
#include <system_error>
#include <utility>

#include "base/path.hpp"

namespace dagent::app {
namespace fs = std::filesystem;
namespace {

using detail::ConfigDocument;
using detail::ModelWriteLock;
using detail::Node;
using detail::env_value;
using detail::expand_env;
using detail::fail;
using detail::parse_file;
using detail::require_private_file;
using detail::size_option;
using json = nlohmann::json;

std::map<std::string, std::string> map_headers(const Node& value, std::string_view kind, bool managed_auth) {
    std::map<std::string, std::string> out;
    const Node headers = value.child("headers");
    if (!headers.exists()) return out;
    if (!headers.raw().is_object()) fail(ConfigError::Kind::type, headers.pointer() + " must be an object");
    for (const auto& [name, text] : headers.raw().items()) {
        if (!text.is_string()) fail(ConfigError::Kind::type, headers.pointer() + " values must be strings");
        out.emplace(name, expand_env(text.get<std::string>(), headers.pointer()));
    }
    try { llm::validate_headers(out, kind, managed_auth); }
    catch (const std::exception& error) { fail(ConfigError::Kind::invalid, headers.pointer() + ": " + error.what()); }
    return out;
}

std::map<std::string, llm::ProviderConfig> map_models(const Node& node) {
    if (!node.has() || !node.raw().is_object() || node.raw().empty())
        fail(ConfigError::Kind::invalid, "models: at least one named model is required");
    std::map<std::string, llm::ProviderConfig> out;
    for (const auto& [name, entry] : node.raw().items()) {
        const std::string key = "/models/" + name;
        if (name.empty() || !entry.is_object()) fail(ConfigError::Kind::type, key + " must be an object");
        const Node value(entry, key);
        llm::ProviderConfig model;
        model.name = name;
        model.kind = value.child("kind").str(model.kind);
        const auto* info = llm::find_provider(model.kind);
        if (!info) fail(ConfigError::Kind::invalid, key + "/kind: unknown provider " + model.kind);
        model.model = value.child("model").str();
        if (model.model.empty()) fail(ConfigError::Kind::invalid, key + "/model must not be empty");
        model.base_url = value.child("base_url").str(std::string(info->default_base_url));
        if (model.base_url.empty()) fail(ConfigError::Kind::invalid, key + "/base_url is required");
        model.max_tokens = size_option(value, "max_tokens");
        model.temperature = value.child("temperature").real(-1.0);
        model.context_window = value.child("context_window").usize();
        model.send_reasoning_content = value.child("send_reasoning_content").flag();
        model.include_usage = value.child("include_usage").flag(true);
        if (const Node extra = value.child("extra_body"); extra.has()) {
            if (!extra.raw().is_object()) fail(ConfigError::Kind::type, key + "/extra_body must be an object");
            model.extra_body = extra.raw();
            const bool output_limit = model.extra_body.contains("max_tokens") ||
                                      model.extra_body.contains("max_completion_tokens") ||
                                      (model.kind == "ollama" && model.extra_body.contains("options") &&
                                       model.extra_body.at("options").is_object() &&
                                       model.extra_body.at("options").contains("num_predict"));
            if (output_limit)
                fail(ConfigError::Kind::invalid, key + "/extra_body: set the output budget with max_tokens");
        }
        if (entry.contains("login") && entry.contains("api_key"))
            fail(ConfigError::Kind::invalid, key + ": login and api_key are mutually exclusive");
        if (entry.contains("login")) {
            if (!entry["login"].is_string() || entry["login"].get_ref<const std::string&>().empty())
                fail(ConfigError::Kind::invalid, key + "/login must name a provider");
            model.login = entry["login"].get<std::string>();
            try { llm::validate_login(model.login, model.kind, model.base_url); }
            catch (const std::exception& error) { fail(ConfigError::Kind::invalid, key + "/login: " + error.what()); }
        }
        model.headers = map_headers(value, model.kind, entry.contains("api_key") || entry.contains("login"));
        const std::string api_key = value.child("api_key").str();
        model.api_key = api_key.starts_with("env:") ? env_value(std::string_view(api_key).substr(4), key + "/api_key")
                                                     : api_key;
        if (info->needs_api_key && model.api_key.empty() && model.login.empty())
            fail(ConfigError::Kind::invalid, key + "/api_key is required");
        if (info->needs_max_tokens && model.max_tokens == 0)
            fail(ConfigError::Kind::invalid, key + "/max_tokens must be greater than zero");
        out.emplace(name, std::move(model));
    }
    return out;
}

std::map<std::string, llm::DecisionProviderConfig> map_decision_models(const Node& node) {
    std::map<std::string, llm::DecisionProviderConfig> out;
    if (!node.has()) return out;
    if (!node.raw().is_object()) fail(ConfigError::Kind::type, node.pointer() + " must be an object");
    for (const auto& [name, entry] : node.raw().items()) {
        const std::string key = node.pointer() + "/" + name;
        if (name.empty() || !entry.is_object()) fail(ConfigError::Kind::type, key + " must be an object");
        const Node value(entry, key);
        llm::DecisionProviderConfig model;
        model.name = name;
        model.kind = value.required("kind").str();
        if (llm::find_decision_provider(model.kind) == nullptr)
            fail(ConfigError::Kind::invalid, key + "/kind: unknown decision provider " + model.kind);
        model.endpoint = value.required("endpoint").str();
        if (model.endpoint.empty()) fail(ConfigError::Kind::invalid, key + "/endpoint must not be empty");
        model.model = value.required("model").str();
        if (model.model.empty()) fail(ConfigError::Kind::invalid, key + "/model must not be empty");
        model.model_revision = value.child("model_revision").str();
        if (entry.contains("login")) fail(ConfigError::Kind::invalid, key + ": decision models do not support login");
        model.headers = map_headers(value, model.kind, false);
        out.emplace(name, std::move(model));
    }
    return out;
}

} // namespace

ModelCatalog load_models(const fs::path& root, const std::vector<std::string>& overrides) {
    const fs::path file = HomePaths(base::canonical_or_absolute_or_lexical(root)).models;
    require_private_file(file);
    const json models_json = parse_file(file);
    std::vector<std::string> model_log;
    std::string selected = Node(models_json, "").child("default").str();
    json models = Node(models_json, "").child("models").has()
                      ? Node(models_json, "").child("models").raw() : json{};
    for (const auto& entry : overrides) {
        if (entry.starts_with("@model=")) {
            const std::string choice = entry.substr(7);
            if (models.is_object() && models.contains(choice)) {
                selected = choice;
                model_log.push_back(std::format("--model {} selected a named configuration", choice));
            } else {
                if (!models.is_object() || !models.contains(selected))
                    fail(ConfigError::Kind::invalid, "select an existing model before overriding its model id");
                models[selected]["model"] = choice;
                model_log.push_back(std::format("--model {} overrides model id in {}", choice, selected));
            }
        }
    }
    ModelCatalog catalog;
    catalog.models = map_models(Node(models, "/models"));
    catalog.selected = std::move(selected);
    catalog.selection_log = std::move(model_log);
    if (!catalog.models.contains(catalog.selected))
        fail(ConfigError::Kind::invalid, "models.json default names an unknown model: " + catalog.selected);

    catalog.decision_models = map_decision_models(Node(models_json, "").child("decision_models"));
    const Node decision = Node(models_json, "").child("decision");
    if (decision.has()) {
        const std::string name = decision.str();
        if (!catalog.decision_models.contains(name))
            fail(ConfigError::Kind::invalid, "/decision names an unknown decision model: " + name);
        catalog.selected_decision = name;
    }
    return catalog;
}

llm::ProviderConfig add_model(const fs::path& root_path,
                                const llm::ProviderConfig& model, bool existing_ok) {
    const fs::path root = base::canonical_or_absolute_or_lexical(root_path);
    const fs::path file = HomePaths(root).models;
    ModelWriteLock lock(root);
    require_private_file(file);
    ConfigDocument document(file);
    auto& models = document.value["models"];
    if (!models.is_object()) fail(ConfigError::Kind::type, "/models must be an object");
    if (model.name.empty()) fail(ConfigError::Kind::invalid, "model configuration name must not be empty");
    if (models.contains(model.name) && existing_ok) return map_models(Node(models, "/models")).at(model.name);
    if (models.contains(model.name))
        fail(ConfigError::Kind::invalid, "model configuration already exists: " + model.name);

    nlohmann::ordered_json entry{{"kind", model.kind},
               {"base_url", model.base_url},
               {"model", model.model},
               {"max_tokens", model.max_tokens},
               {"context_window", model.context_window},
               {"send_reasoning_content", model.send_reasoning_content},
               {"include_usage", model.include_usage}};
    if (!model.api_key.empty()) entry["api_key"] = model.api_key;
    if (!model.login.empty()) entry["login"] = model.login;
    if (model.temperature >= 0.0) entry["temperature"] = model.temperature;
    if (!model.extra_body.empty()) entry["extra_body"] = model.extra_body;
    if (!model.headers.empty()) entry["headers"] = model.headers;
    models[model.name] = std::move(entry);

    const auto parsed = map_models(Node(models, "/models"));
    const auto selected = parsed.find(model.name);
    if (selected == parsed.end()) fail(ConfigError::Kind::invalid, "failed to add model " + model.name);

    const std::optional<workspace::Stamp> stamp = workspace::stamp_of(file);
    workspace::write_text(file, document.dump(), workspace::Eol::lf, false, stamp);
    require_private_file(file);
    return selected->second;
}

} // namespace dagent::app
