#include "llm/auth.hpp"
#include "base/platform.hpp"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <mutex>
#include <random>
#include <utility>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <curl/curl.h>

namespace dagent::llm {
namespace {
using json = nlohmann::json;
using namespace std::chrono_literals;
struct Fd {
    int value = -1;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
[[noreturn]] void io_error() { throw std::runtime_error("credential storage is unavailable or unsafe"); }
[[noreturn]] void sign_in() { throw std::runtime_error("OpenCode needs sign-in; use dagent login opencode or /model"); }
void check_stop(std::stop_token stop) {
    if (stop.stop_requested()) throw net::HttpError(net::HttpError::Kind::cancelled, "authentication cancelled");
}
void pause(std::chrono::steady_clock::duration duration, std::stop_token stop) {
    std::mutex mutex;
    std::condition_variable_any cv;
    std::unique_lock lock(mutex);
    cv.wait_for(lock, stop, duration, [] { return false; });
    check_stop(stop);
}
void check_file(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0777) != 0600)
        io_error();
}
int directory(const std::filesystem::path& path, bool create) {
    Fd current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (current.value < 0) io_error();
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    for (const auto& part : absolute.relative_path()) {
        int next = ::openat(current.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT) {
            if (!create) return -1;
            if (::mkdirat(current.value, part.c_str(), 0700) && errno != EEXIST) io_error();
            next = ::openat(current.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) io_error();
        ::close(current.value);
        current.value = next;
    }
    struct stat st{};
    if (::fstat(current.value, &st) || st.st_uid != ::geteuid() || (st.st_mode & 0777) != 0700) io_error();
    return std::exchange(current.value, -1);
}
struct CredentialLock {
    Fd dir, file;
    CredentialLock(const CredentialPaths& paths, std::stop_token stop)
        : dir(directory(paths.lock.parent_path(), true)),
          file(::openat(dir.value, paths.lock.filename().c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600)) {
        if (file.value < 0) io_error();
        check_file(file.value);
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        for (;;) {
            check_stop(stop);
            if (::flock(file.value, LOCK_EX | LOCK_NB) == 0) break;
            if (errno != EWOULDBLOCK && errno != EINTR) io_error();
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("credential lock wait timed out");
            pause(50ms, stop);
        }
    }
};
json credential_json(const Credentials& c) {
    return {{"access_token", c.access_token}, {"refresh_token", c.refresh_token}, {"expires_at", c.expires_at},
            {"org_id", c.org_id}, {"client_id", c.client_id}, {"state", c.state}, {"updated", c.updated}};
}
void write_credentials(const CredentialPaths& paths, const Credentials& value) {
    Fd dir(directory(paths.file.parent_path(), true));
    const auto name = paths.file.filename().string();
    Fd existing(::openat(dir.value, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (existing.value >= 0) check_file(existing.value);
    else if (errno != ENOENT) io_error();
    const auto temp = std::format(".{}-{}-{}", name, base::process_id(), std::random_device{}());
    Fd fd(::openat(dir.value, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (fd.value < 0) io_error();
    try {
        const auto data = credential_json(value).dump() + "\n";
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto n = ::write(fd.value, data.data() + offset, data.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) io_error();
            offset += static_cast<std::size_t>(n);
        }
        if (::fsync(fd.value) || ::renameat(dir.value, temp.c_str(), dir.value, name.c_str()) || ::fsync(dir.value)) io_error();
    } catch (...) { ::unlinkat(dir.value, temp.c_str(), 0); throw; }
}
struct Url {
    CURLU* value = curl_url();
    ~Url() { curl_url_cleanup(value); }
    std::string get(CURLUPart part) const {
        char* raw = nullptr;
        if (curl_url_get(value, part, &raw, 0) != CURLUE_OK) return {};
        std::string text(raw);
        curl_free(raw);
        return text;
    }
};
void parse_url(Url& url, std::string_view text) {
    if (text.find('\0') != std::string_view::npos ||
        curl_url_set(url.value, CURLUPART_URL, std::string(text).c_str(), 0) != CURLUE_OK)
        throw std::runtime_error("invalid login provider URL");
}
std::string origin(const Url& url) {
    const auto port = url.get(CURLUPART_PORT);
    char* raw = nullptr;
    const bool user = curl_url_get(url.value, CURLUPART_USER, &raw, 0) == CURLUE_OK;
    curl_free(raw);
    if (user || url.get(CURLUPART_SCHEME) != "https" || !url.get(CURLUPART_PASSWORD).empty() ||
        !url.get(CURLUPART_OPTIONS).empty()) throw std::runtime_error("unsafe login provider URL");
    auto host = url.get(CURLUPART_HOST);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; });
    return "https://" + host + (port.empty() || port == "443" ? "" : ":" + port);
}
std::string approval_url(const LoginProvider& p, std::string_view link) {
    Url url;
    parse_url(url, p.approval_base);
    parse_url(url, link); // curl resolves relative links against the existing URL.
    if (origin(url) != p.origins.front()) throw std::runtime_error("unsafe approval URL");
    return url.get(CURLUPART_URL);
}
std::string form(const json& fields) {
    std::string out;
    for (const auto& [key, item] : fields.items()) {
        const auto value = item.is_string() ? item.get<std::string>() : item.dump();
        char* encoded = curl_easy_escape(nullptr, value.c_str(), static_cast<int>(value.size()));
        if (!encoded) throw std::runtime_error("cannot encode authentication request");
        if (!out.empty()) out += '&';
        out += key + "=" + encoded;
        curl_free(encoded);
    }
    return out;
}
struct OAuthReply { long status; json body; };
OAuthReply oauth(const LoginProvider& p, std::string_view url, const json& fields, std::stop_token stop,
                 std::optional<std::chrono::steady_clock::time_point> deadline = {}) {
    net::HttpOptions options;
    options.timeout = 30s;
    options.connect_timeout = 10s;
    options.max_body_bytes = 64 * 1024;
    net::HttpClient client(options);
    net::HttpRequest request;
    request.url = checked_login_url(p, url, true);
    request.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}};
    request.body = form(fields);
    net::HttpResponse response;
    try { response = client.send(request, stop, deadline); }
    catch (const net::HttpError& error) {
        throw net::HttpError(error.kind(), "authentication transport failed; try signing in again");
    }
    auto body = json::parse(response.body, nullptr, false);
    if (!body.is_object()) throw std::runtime_error(std::format("OAuth HTTP {}: invalid_response; sign in again", response.status));
    return {response.status, std::move(body)};
}
std::string error_code(const OAuthReply& response) {
    const auto it = response.body.find("error");
    if (it == response.body.end() || !it->is_string()) return "invalid_response";
    const auto code = it->get<std::string>();
    for (const auto allowed : {"authorization_pending", "slow_down", "access_denied", "expired_token", "invalid_grant", "invalid_client", "invalid_request", "unsupported_grant_type"})
        if (code == allowed) return code;
    return "oauth_error";
}
[[noreturn]] void oauth_error(const OAuthReply& r) {
    throw std::runtime_error(std::format("OAuth HTTP {}: {}; sign in again", r.status, error_code(r)));
}
Credentials tokens(const LoginProvider& p, const OAuthReply& r, std::string_view org = {}) {
    if (r.status < 200 || r.status >= 300 || r.body.contains("error")) oauth_error(r);
    try {
        Credentials c;
        c.access_token = r.body.at("access_token").get<std::string>();
        c.refresh_token = r.body.at("refresh_token").get<std::string>();
        c.org_id = r.body.value("org_id", std::string(org));
        c.client_id = p.client_id;
        c.updated = auth_now();
        const auto expires = r.body.at("expires_in").get<std::int64_t>();
        if (c.access_token.empty() || c.refresh_token.empty() || expires <= 0 || expires > 366LL * 86400) sign_in();
        c.expires_at = c.updated + expires;
        c.state = "ready";
        return c;
    } catch (const json::exception&) { throw std::runtime_error("invalid OAuth token response; sign in again"); }
}
} // namespace

std::int64_t auth_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
const LoginProvider& login_provider(std::string_view id) {
    static const LoginProvider opencode{
        "opencode", "https://opencode.ai/console/auth/device/code", "https://opencode.ai/console/auth/device/token",
        "dagent", "https://opencode.ai", "https://opencode.ai/go", true,
        {"https://opencode.ai"}, {"/inference/go/openai/v1", "/inference/go/anthropic/v1"},
        {"openai-chat"}, {{"x-opencode-session", "{session_id}"}}, "https://models.dev/api.json", "opencode-go"};
    if (id != opencode.id) throw std::invalid_argument("unknown login provider");
    return opencode;
}
std::string checked_login_url(const LoginProvider& p, std::string_view text, bool auth_endpoint) {
    Url url;
    parse_url(url, text);
    const auto host = origin(url);
    if (std::find(p.origins.begin(), p.origins.end(), host) == p.origins.end() ||
        !url.get(CURLUPART_QUERY).empty() || !url.get(CURLUPART_FRAGMENT).empty())
        throw std::invalid_argument("URL is outside the login provider allowlist");
    const auto path = url.get(CURLUPART_PATH);
    bool allowed = false;
    if (auth_endpoint) {
        allowed = host + path == p.device_endpoint || host + path == p.token_endpoint;
    } else {
        for (const auto& base : p.inference_paths)
            if (path == base || path == base + "/" || path == base + "/chat/completions" || path == base + "/messages") allowed = true;
    }
    if (!allowed) throw std::invalid_argument("URL path is outside the login provider allowlist");
    return url.get(CURLUPART_URL);
}
void validate_login(std::string_view id, std::string_view kind, std::string_view base_url) {
    const auto& p = login_provider(id);
    if (std::find(p.kinds.begin(), p.kinds.end(), kind) == p.kinds.end())
        throw std::invalid_argument("protocol is unavailable for this login provider");
    checked_login_url(p, base_url);
    Url url;
    parse_url(url, base_url);
    auto path = url.get(CURLUPART_PATH);
    if (path.ends_with('/')) path.pop_back();
    if (kind == "openai-chat" && path != "/inference/go/openai/v1")
        throw std::invalid_argument("base_url does not match the login protocol");
}
std::optional<Credentials> read_credentials(const CredentialPaths& paths) {
    Fd dir(directory(paths.file.parent_path(), false));
    if (dir.value < 0) return std::nullopt;
    Fd fd(::openat(dir.value, paths.file.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (fd.value < 0) { if (errno == ENOENT) return std::nullopt; io_error(); }
    check_file(fd.value);
    std::string data;
    char buffer[4096];
    for (;;) {
        const auto n = ::read(fd.value, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || data.size() + static_cast<std::size_t>(n) > 64 * 1024) io_error();
        if (n == 0) break;
        data.append(buffer, static_cast<std::size_t>(n));
    }
    try {
        const auto j = json::parse(data);
        Credentials c{j.at("access_token").get<std::string>(), j.at("refresh_token").get<std::string>(),
                      j.at("org_id").get<std::string>(), j.at("client_id").get<std::string>(),
                      j.at("state").get<std::string>(), j.at("expires_at").get<std::int64_t>(), j.at("updated").get<std::int64_t>()};
        return c;
    } catch (const json::exception&) {
        Credentials invalid;
        invalid.state = "invalid";
        return invalid;
    }
}
json credential_status(const CredentialPaths& paths) {
    const auto c = read_credentials(paths);
    if (!c) return {{"provider", "opencode"}, {"state", "signed_out"}};
    const auto workspace = c->org_id == c->access_token || c->org_id == c->refresh_token ? "[redacted]" : c->org_id;
    return {{"provider", "opencode"}, {"state", c->state == "ready" && c->client_id == "dagent" && !c->access_token.empty() && !c->refresh_token.empty() && c->expires_at > auth_now() ? "signed_in" : "needs_sign_in"},
            {"workspace", workspace}, {"expires_at", c->expires_at}};
}
DeviceLogin start_device_login(const LoginProvider& p, std::stop_token stop) {
    const auto r = oauth(p, p.device_endpoint, {{"client_id", p.client_id}, {"supports_org_scope", p.supports_org_scope}}, stop);
    if (r.status < 200 || r.status >= 300 || r.body.contains("error")) oauth_error(r);
    try {
        DeviceLogin d;
        d.device_code = r.body.at("device_code").get<std::string>();
        d.user_code = r.body.at("user_code").get<std::string>();
        const auto uri = r.body.value("verification_uri_complete", r.body.at("verification_uri").get<std::string>());
        d.verification_url = approval_url(p, uri);
        const auto seconds = r.body.at("expires_in").get<int>();
        const auto interval = r.body.value("interval", 5);
        if (seconds <= 0 || seconds > 86400 || interval <= 0 || interval > 86400 || d.device_code.empty() || d.user_code.empty())
            throw std::runtime_error("invalid OAuth device response");
        d.expires_at = auth_now() + seconds;
        d.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        d.interval = std::chrono::seconds(interval);
        return d;
    } catch (const json::exception&) { throw std::runtime_error("invalid OAuth device response"); }
}
Credentials wait_device_login(const LoginProvider& p, const CredentialPaths& paths, DeviceLogin d, std::stop_token stop) {
    while (std::chrono::steady_clock::now() < d.deadline) {
        pause(std::min(std::chrono::steady_clock::duration(d.interval), d.deadline - std::chrono::steady_clock::now()), stop);
        if (std::chrono::steady_clock::now() >= d.deadline) break;
        OAuthReply r;
        try {
            r = oauth(p, p.token_endpoint, {{"client_id", p.client_id}, {"device_code", d.device_code},
                      {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}}, stop, d.deadline);
        } catch (const net::HttpError& e) {
            if (e.kind() != net::HttpError::Kind::timeout) throw;
            d.interval = std::min(d.interval * 2, 3600s);
            continue;
        }
        if (r.body.contains("error")) {
            const auto code = error_code(r);
            if (code == "slow_down") { d.interval += 5s; continue; }
            if (code == "authorization_pending") continue;
            oauth_error(r);
        }
        auto c = tokens(p, r);
        CredentialLock lock(paths, stop);
        check_stop(stop);
        write_credentials(paths, c);
        return c;
    }
    throw std::runtime_error("OAuth expired_token: device approval expired; sign in again");
}
void logout(const CredentialPaths& paths, std::stop_token stop) {
    CredentialLock lock(paths, stop);
    if (!read_credentials(paths)) return;
    Fd dir(directory(paths.file.parent_path(), false));
    if (::unlinkat(dir.value, paths.file.filename().c_str(), 0) || ::fsync(dir.value)) io_error();
}
Credentials request_credentials(const LoginProvider& p, const CredentialPaths& paths, std::stop_token stop,
                                std::string_view previous_token) {
    auto c = read_credentials(paths);
    if (!c || c->state != "ready" || c->client_id != p.client_id || c->access_token.empty() || c->refresh_token.empty()) sign_in();
    if (previous_token.empty() && c->expires_at > auth_now() + 300) return *c;
    const auto observed = previous_token.empty() ? c->access_token : std::string(previous_token);
    CredentialLock lock(paths, stop);
    c = read_credentials(paths);
    if (!c || c->state != "ready" || c->client_id != p.client_id || c->access_token.empty() || c->refresh_token.empty()) sign_in();
    if (c->access_token != observed || (previous_token.empty() && c->expires_at > auth_now() + 300)) return *c;
    check_stop(stop);
    c->state = "refresh_pending";
    c->updated = auth_now();
    write_credentials(paths, *c);
    const auto response = oauth(p, p.token_endpoint, {{"client_id", p.client_id}, {"grant_type", "refresh_token"},
                                {"refresh_token", c->refresh_token}}, stop);
    auto refreshed = tokens(p, response, c->org_id);
    write_credentials(paths, refreshed);
    return refreshed;
}
} // namespace dagent::llm
