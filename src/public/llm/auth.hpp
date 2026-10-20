#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "lib/nlohmann/json.hpp"
#include "net/http.hpp"

namespace dagent::llm {

struct LoginProvider {
    std::string id, device_endpoint, token_endpoint, client_id, approval_base, subscription_url;
    bool supports_org_scope;
    std::vector<std::string> origins, inference_paths, kinds;
    std::map<std::string, std::string> headers;
    std::string models_url, models_provider;
};
const LoginProvider& login_provider(std::string_view id);
// Uses the same URL parser as the HTTP transport; exact origin and path checks.
std::string checked_login_url(const LoginProvider&, std::string_view url, bool token_endpoint = false);
void validate_login(std::string_view id, std::string_view kind, std::string_view base_url);

struct CredentialPaths { std::filesystem::path file, lock; };
struct Credentials {
    std::string access_token, refresh_token, org_id, client_id, state;
    std::int64_t expires_at = 0, updated = 0;
};
struct DeviceLogin {
    std::string device_code, user_code, verification_url;
    std::int64_t expires_at = 0;
    std::chrono::seconds interval{5};
    std::chrono::steady_clock::time_point deadline;
};
std::optional<Credentials> read_credentials(const CredentialPaths&);
nlohmann::json credential_status(const CredentialPaths&);
DeviceLogin start_device_login(const LoginProvider&, std::stop_token);
Credentials wait_device_login(const LoginProvider&, const CredentialPaths&, DeviceLogin, std::stop_token);
void logout(const CredentialPaths&, std::stop_token);
// previous_token is supplied only for the one HTTP 401 recovery.
Credentials request_credentials(const LoginProvider&, const CredentialPaths&, std::stop_token,
                                std::string_view previous_token = {});
std::int64_t auth_now();
} // namespace dagent::llm
