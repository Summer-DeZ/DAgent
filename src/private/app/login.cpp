#include "app/login.hpp"

#include <algorithm>
#include <fstream>
#include "app/config.hpp"
#include "llm/llm.hpp"
#include "workspace/files.hpp"

namespace dagent::app {
namespace {
using json = nlohmann::json;
json read_json(const std::filesystem::path& file) {
    std::ifstream input(file);
    if (!input) throw std::runtime_error("cannot read provider model data");
    auto value = json::parse(input, nullptr, false);
    if (!value.is_object()) throw std::runtime_error("invalid provider model data");
    return value;
}
llm::ProviderConfig model_config(const json& item) {
    llm::ProviderConfig config;
    config.name = item.at("id").get<std::string>();
    config.model = item.at("model").get<std::string>();
    config.kind = item.at("kind").get<std::string>();
    config.base_url = item.at("base_url").get<std::string>();
    config.context_window = item.at("context_window").get<std::size_t>();
    config.max_tokens = item.at("max_tokens").get<std::size_t>();
    config.send_reasoning_content = item.at("send_reasoning_content").get<bool>();
    config.login = "opencode";
    return config;
}
} // namespace

llm::CredentialPaths Login::credentials() const {
    return {paths_.credentials / "opencode.json", paths_.credential_locks / "opencode.lock"};
}
bool Login::cancel(std::string_view id) {
    const std::lock_guard lock(mutex_);
    if (!flow_ || (!id.empty() && flow_->id != id)) return false;
    flow_->stop.request_stop();
    return true;
}
json Login::models(bool refresh, std::stop_token stop) {
    const auto& provider = llm::login_provider("opencode");
    json cache;
    try { cache = read_json(paths_.model_cache); }
    catch (const std::exception&) { cache = nullptr; }
    auto fetched = cache.is_object() ? cache.value("fetched_at", std::int64_t{0}) : 0;
    bool stale = false;
    if (refresh || fetched <= 0 || llm::auth_now() - fetched >= 86400) {
        try {
            net::HttpOptions options;
            options.timeout = std::chrono::seconds(30);
            options.connect_timeout = std::chrono::seconds(10);
            options.max_body_bytes = 32 * 1024 * 1024;
            net::HttpClient client(options);
            net::HttpRequest request;
            request.method = "GET";
            request.url = provider.models_url;
            const auto response = client.send(request, stop);
            if (response.status != 200) throw std::runtime_error("model catalog download failed");
            const auto document = json::parse(response.body, nullptr, false);
            if (!document.is_object() || !document.contains(provider.models_provider) ||
                !document.at(provider.models_provider).is_object() ||
                !document.at(provider.models_provider).contains("models") ||
                !document.at(provider.models_provider).at("models").is_object())
                throw std::runtime_error("invalid provider model catalog");
            fetched = llm::auth_now();
            cache = {{"fetched_at", fetched}, {"provider", document.at(provider.models_provider)}};
            std::filesystem::create_directories(paths_.model_cache.parent_path());
            workspace::write_text(paths_.model_cache, cache.dump() + "\n", workspace::Eol::lf, false,
                                  workspace::stamp_of(paths_.model_cache));
        } catch (const std::exception&) {
            if (stop.stop_requested()) throw std::runtime_error("model catalog request cancelled");
            if (cache.is_null()) throw std::runtime_error("model catalog unavailable; connect and refresh again");
            stale = true;
        }
    }
    const auto configured = read_json(paths_.models).value("models", json::object());
    const auto& catalog = cache.at("provider");
    json rows = json::array();
    for (const auto& [key, model] : catalog.at("models").items()) {
        if (!model.is_object()) continue;
        try {
            const auto metadata = model.value("provider", json::object());
            const auto npm = metadata.value("npm", catalog.value("npm", "@ai-sdk/openai-compatible"));
            if (npm != "@ai-sdk/anthropic" && npm != "@ai-sdk/openai-compatible" && npm != "@ai-sdk/openai") continue;
            const bool anthropic = npm == "@ai-sdk/anthropic";
            const auto fallback = anthropic ? "https://opencode.ai/inference/go/anthropic/v1" : "https://opencode.ai/inference/go/openai/v1";
            const auto base = metadata.value("api", catalog.value("api", std::string(fallback)));
            try { llm::checked_login_url(provider, base); }
            catch (const std::exception&) { continue; }
            const bool anthropic_endpoint = base.find("/anthropic/") != std::string::npos;
            const auto kind = anthropic || anthropic_endpoint ? "anthropic" : "openai-chat";
            const auto id = model.value("id", key);
            const auto limit = model.at("limit");
            const auto context = limit.at("context").get<std::size_t>();
            if (!limit.at("context").is_number_integer() || !limit.at("output").is_number_integer() ||
                limit.at("context").get<std::int64_t>() <= 0 || limit.at("output").get<std::int64_t>() <= 0) continue;
            const auto output = std::min(limit.at("output").get<std::size_t>(), std::size_t{32768});
            if (!context || !output || id.empty()) continue;
            const std::string name = "opencode-go/" + id;
            rows.push_back({{"id", name}, {"model", id}, {"name", model.value("name", id)},
                            {"kind", kind}, {"base_url", base}, {"context_window", context}, {"max_tokens", output},
                            {"send_reasoning_content", model.value("reasoning", false)},
                            {"cost", model.value("cost", json::object())}, {"available", kind == std::string_view("openai-chat")},
                            {"unavailable_reason", kind == std::string_view("anthropic") ? "Anthropic login endpoint is not yet verified" : ""},
                            {"added", configured.contains(name)}});
        } catch (const json::exception&) { continue; }
    }
    return {{"provider", "opencode"}, {"models", std::move(rows)}, {"cache_age_seconds", std::max(std::int64_t{0}, llm::auth_now() - fetched)},
            {"stale", stale || llm::auth_now() - fetched >= 86400}};
}
json Login::invoke(std::string_view method, const json& params, std::stop_token stop) {
    const auto& provider = llm::login_provider(params.at("provider").get<std::string>());
    if (method == "provider.status") return llm::credential_status(credentials());
    if (method == "provider.logout") {
        cancel({});
        llm::logout(credentials(), stop);
        return {{"provider", provider.id}, {"state", "signed_out"}};
    }
    if (method == "provider.login.start") {
        {
            const std::lock_guard lock(mutex_);
            if (flow_ && !flow_->stop.stop_requested() && flow_->device.deadline > std::chrono::steady_clock::now())
                throw std::runtime_error("a provider login is already pending");
        }
        auto flow = std::make_shared<Flow>();
        flow->device = llm::start_device_login(provider, stop);
        {
            const std::lock_guard lock(mutex_);
            flow->id = "login-" + std::to_string(++serial_);
            flow_ = flow;
        }
        return {{"login_id", flow->id}, {"provider", provider.id}, {"verification_url", flow->device.verification_url},
                {"user_code", flow->device.user_code}, {"expires_at", flow->device.expires_at},
                {"subscription_url", provider.subscription_url}};
    }
    if (method == "provider.login.wait") {
        std::shared_ptr<Flow> flow;
        {
            const std::lock_guard lock(mutex_);
            if (!flow_ || flow_->id != params.at("login_id").get<std::string>())
                throw std::runtime_error("unknown provider login");
            flow = flow_;
        }
        std::stop_callback cancelled(stop, [&] { flow->stop.request_stop(); });
        try { llm::wait_device_login(provider, credentials(), flow->device, flow->stop.get_token()); }
        catch (...) {
            const std::lock_guard lock(mutex_);
            flow_.reset();
            throw;
        }
        { const std::lock_guard lock(mutex_); flow_.reset(); }
        return llm::credential_status(credentials());
    }
    if (method == "provider.models.list") return models(params.value("refresh", false), stop);
    if (method == "provider.model.add") {
        const auto id = params.at("model_id").get<std::string>();
        // Existing entries switch directly, including while the public catalog is offline.
        auto existing = load_models(paths_.root, {});
        if (existing.models.contains(id)) {
            if (!id.starts_with("opencode-go/")) throw std::runtime_error("invalid provider model id");
            return {{"name", id}};
        }
        const auto list = models(false, stop);
        for (const auto& item : list.at("models")) {
            if (item.at("id") != id) continue;
            if (!item.at("available").get<bool>()) throw std::runtime_error("this provider model is not available yet");
            const auto saved = add_model(paths_.root, model_config(item), true);
            return {{"name", saved.name}};
        }
        throw std::runtime_error("provider model not found");
    }
    throw std::invalid_argument("unknown provider operation");
}
} // namespace dagent::app
