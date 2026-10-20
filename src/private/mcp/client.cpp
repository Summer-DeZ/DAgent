#include "mcp/client.hpp"
#include "mcp/detail.hpp"
#include "mcp/transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/log.hpp"

namespace dagent::mcp {
namespace {

using json = nlohmann::json;
using std::chrono::milliseconds;
using detail::Response;

constexpr std::size_t kMaxQualifiedToolName = 64; ///< OpenAI 等协议对函数名的长度限制

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

void set_header(net::Headers& headers, std::string_view name, std::string value) {
    for (auto& [key, existing] : headers) {
        if (iequals(key, name)) {
            existing = std::move(value);
            return;
        }
    }
    headers.emplace_back(std::string(name), std::move(value));
}

std::string string_or(const json& object, const char* key, std::string fallback = {}) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::move(fallback);
}

bool bool_or(const json& object, const char* key, bool fallback = false) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

int int_or(const json& object, const char* key, int fallback = 0) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<int>() : fallback;
}

json member_or(const json& object, const char* key, json fallback) {
    const auto it = object.find(key);
    return it != object.end() ? *it : std::move(fallback);
}

std::string rpc_text(std::string_view method, const json& error) {
    std::string message = "unknown error";
    int code = 0;
    if (error.is_object()) {
        code = int_or(error, "code");
        message = string_or(error, "message", message);
    }
    return std::format("{} returned an error: {} ({})", method, message, code);
}

/// @brief 按 HTTP 状态码报错；401/403 是配置问题，5xx 可重试。error 是响应体里的 JSON-RPC error（可为
/// null），有 message 时附在后面。
[[noreturn]] void throw_http_status(const std::string& server, std::string_view method, long status,
                                    const json& error = nullptr) {
    std::string detail;
    if (error.is_object()) {
        if (const std::string message = string_or(error, "message"); !message.empty())
            detail = std::format("：{}", message);
    }
    if (status == 401 || status == 403)
        throw McpError{McpError::Kind::handshake,
                       std::format("{}: {} denied (HTTP {}); check authentication headers in the server config{}", server, method,
                                   status, detail)};
    if (status >= 500)
        throw McpError{McpError::Kind::disconnected,
                       std::format("{}: {} failed (HTTP {}); server error{}", server, method, status, detail)};
    throw McpError{McpError::Kind::protocol,
                   std::format("{}: {} failed (HTTP {}); response is not JSON-RPC{}", server, method, status,
                               detail)};
}

enum class DialectKind { stateless, session };

struct Revision {
    std::string_view version;
    DialectKind dialect;
    std::string_view handshake_method;
    bool request_meta;
    bool http_session;
    bool version_header;
    bool routing_headers;
};

// 同一 dialect 的条目按偏好排列；握手提议首选版本，服务端可协商到同组其他版本。
constexpr Revision kRevisions[] = {
    {"2026-07-28", DialectKind::stateless, "server/discover", true, false, true, true},
    {"2025-11-25", DialectKind::session, "initialize", false, true, true, false},
    {"2025-06-18", DialectKind::session, "initialize", false, true, true, false},
    {"2025-03-26", DialectKind::session, "initialize", false, true, false, false},
};

} // namespace

struct Client::Impl {
    struct Dialect {
        explicit Dialect(const Revision& revision) : revision_(&revision) {}
        virtual ~Dialect() = default;
        const Revision& revision() const { return *revision_; }
        // false 只表示服务端明确不支持该握手；其他失败直接抛出。
        virtual bool handshake(Impl& client, std::stop_token stop) = 0;
        json decorate_params(json params) const;
        virtual net::Headers headers(const Impl& client, std::string_view method, const json& params) const;
        virtual void capture_session(std::string_view, const net::HttpResponse&) {}
        virtual bool session_expired(const Response&) const { return false; }
        virtual void close(Impl&) const {}

    protected:
        const Revision* revision_;
    };

    struct StatelessDialect final : Dialect {
        using Dialect::Dialect;
        bool handshake(Impl& client, std::stop_token stop) override;
    };

    struct SessionDialect final : Dialect {
        using Dialect::Dialect;
        bool handshake(Impl& client, std::stop_token stop) override;
        net::Headers headers(const Impl& client, std::string_view method, const json& params) const override;
        void capture_session(std::string_view method, const net::HttpResponse& response) override;
        bool session_expired(const Response& response) const override;
        void close(Impl& client) const override;

        std::string session_id_; // 仅 HTTP，握手响应头提供
    };

    Impl(ServerConfig config, Options opt) : config_(std::move(config)), opt_(std::move(opt)) {}
    ~Impl() { shutdown(); }

    // ---- 生命周期
    void start();
    void shutdown();
    void negotiate(std::stop_token stop);
    void close_http_session(const net::Headers& headers);
    void fetch_tools(milliseconds timeout, std::stop_token stop);

    // ---- 消息收发
    Response exchange(std::string_view method, json params, milliseconds timeout, std::stop_token stop,
                      bool notification = false);
    Response http_exchange(std::optional<std::int64_t> id, std::string_view method, json params, milliseconds timeout,
                           std::stop_token stop);
    json request(std::string_view method, json params, milliseconds timeout, std::stop_token stop,
                 bool notification = false);

    ServerConfig config_;
    Options opt_;
    std::unique_ptr<Dialect> dialect_;
    std::string protocol_version_; ///< 握手完成后不再改变

    std::atomic<std::int64_t> next_id_{1};
    detail::ConnectionState connection_;
    std::unique_ptr<detail::StdioTransport> stdio_;
    std::unique_ptr<detail::HttpTransport> http_transport_;

    mutable std::mutex data_mu_;
    std::vector<Tool> tools_;
    std::map<std::string, std::vector<detail::HeaderParam>> header_params_;

};

void Client::Impl::start() {
    if (config_.name.empty()) throw McpError{McpError::Kind::handshake, "server name is empty"};
    if (config_.transport == Transport::http) {
        http_transport_ = std::make_unique<detail::HttpTransport>(config_, opt_, connection_);
        http_transport_->start();
        return;
    }
    stdio_ = std::make_unique<detail::StdioTransport>(config_, opt_, connection_);
    stdio_->start();
}

void Client::Impl::shutdown() {
    if (stdio_) {
        stdio_->shutdown([this] { if (dialect_) dialect_->close(*this); });
    } else if (http_transport_) {
        http_transport_->shutdown([this] { if (dialect_) dialect_->close(*this); });
    }
}

void Client::Impl::close_http_session(const net::Headers& headers) {
    http_transport_->close_session(headers);
}

// ---- Dialect 的请求装饰与会话 ----

json Client::Impl::Dialect::decorate_params(json params) const {
    if (!params.is_object()) params = json::object();
    if (revision().request_meta) params["_meta"] = detail::modern_meta(revision().version);
    return params;
}

net::Headers Client::Impl::Dialect::headers(const Impl& client, std::string_view method, const json& params) const {
    net::Headers headers = client.config_.headers;
    set_header(headers, "Accept", "application/json, text/event-stream");
    set_header(headers, "Content-Type", "application/json");
    if (revision().version_header)
        set_header(headers, "MCP-Protocol-Version", std::string(revision().version));
    else
        std::erase_if(headers, [](const auto& header) { return iequals(header.first, "MCP-Protocol-Version"); });
    if (!revision().routing_headers) return headers;
    set_header(headers, "Mcp-Method", std::string(method));
    if (method == "tools/call" && params.is_object()) {
        const std::string name = string_or(params, "name");
        set_header(headers, "Mcp-Name", detail::encode_header_value(name));
        std::lock_guard lock(client.data_mu_);
        const auto it = client.header_params_.find(name);
        if (it != client.header_params_.end()) {
            const json arguments = params.contains("arguments") ? params["arguments"] : json::object();
            for (auto& [header, value] : detail::header_param_values(it->second, arguments))
                set_header(headers, header, std::move(value));
        }
    }
    return headers;
}

net::Headers Client::Impl::SessionDialect::headers(const Impl& client, std::string_view method,
                                                  const json& params) const {
    auto headers = Dialect::headers(client, method, params);
    if (revision().http_session && !session_id_.empty()) set_header(headers, "Mcp-Session-Id", session_id_);
    return headers;
}

void Client::Impl::SessionDialect::capture_session(std::string_view method, const net::HttpResponse& response) {
    if (revision().http_session && method == revision().handshake_method &&
        response.status >= 200 && response.status < 300) {
        if (const auto session = response.header("mcp-session-id")) session_id_ = *session;
    }
}

bool Client::Impl::SessionDialect::session_expired(const Response& response) const {
    return revision().http_session && response.status == 404 && !session_id_.empty();
}

void Client::Impl::SessionDialect::close(Impl& client) const {
    if (client.config_.transport == Transport::http && revision().http_session && !session_id_.empty())
        client.close_http_session(headers(client, {}, json::object()));
}

// ---- 共享传输与请求 ----

Response Client::Impl::exchange(std::string_view method, json params, milliseconds timeout,
                                std::stop_token stop, bool notification) {
    {
        // 被显式撤销后，旧 lease 不能再发请求。
        const std::lock_guard lock(connection_.mutex);
        if (connection_.closing || connection_.disconnected)
            throw McpError{McpError::Kind::disconnected, config_.name + ": client is closed"};
    }
    const std::optional<std::int64_t> id = notification ? std::nullopt
                                                       : std::optional{next_id_.fetch_add(1)};
    if (config_.transport == Transport::http) return http_exchange(id, method, std::move(params), timeout, stop);
    if (notification) return stdio_->notify(method, std::move(params), stop);
    return stdio_->exchange(*id, method, dialect_->decorate_params(std::move(params)), timeout, stop);
}

Response Client::Impl::http_exchange(std::optional<std::int64_t> id, std::string_view method, json params,
                                     milliseconds timeout, std::stop_token stop) {
    const json message = id ? detail::make_request(*id, method, dialect_->decorate_params(std::move(params)))
                            : detail::make_notification(method, std::move(params));
    return http_transport_->exchange(id, method, message, timeout, stop,
        [this, method, &message] { return dialect_->headers(*this, method, message["params"]); },
        [this, method](const net::HttpResponse& headers) { dialect_->capture_session(method, headers); });
}

json Client::Impl::request(std::string_view method, json params, milliseconds timeout, std::stop_token stop,
                           bool notification) {
    Response response = exchange(method, std::move(params), timeout, stop, notification);
    if (dialect_->session_expired(response)) {
        const std::lock_guard lock(connection_.mutex);
        connection_.disconnected = true;
        connection_.disconnect_reason = config_.name + ": HTTP session expired (HTTP 404)";
        throw McpError{McpError::Kind::disconnected, connection_.disconnect_reason};
    }
    // 鉴权和 server 侧错误按状态码分类，哪怕 body 里带着 JSON-RPC error。
    if (response.status == 401 || response.status == 403 || response.status >= 500)
        throw_http_status(config_.name, method, response.status, response.error);
    if (response.rpc_error)
        throw McpError{McpError::Kind::rpc, config_.name + ": " + rpc_text(method, response.error)};
    if (!response.ok) {
        if (response.status >= 400) throw_http_status(config_.name, method, response.status);
        throw McpError{McpError::Kind::protocol,
                       std::format("{}: {} response contains neither result nor error", config_.name, method)};
    }
    return std::move(response.result);
}

// ---- Dialect 握手与协商 ----

bool Client::Impl::StatelessDialect::handshake(Impl& client, std::stop_token stop) {
    const auto method = revision().handshake_method;
    const Response response = client.exchange(method, json::object(),
                                               client.config_.transport == Transport::http
                                                   ? client.opt_.connect_timeout : client.opt_.probe_timeout,
                                               stop);
    if (response.status == 401 || response.status == 403 || response.status >= 500)
        throw_http_status(client.config_.name, method, response.status, response.error);
    if ((response.rpc_error && int_or(response.error, "code") == -32601) ||
        (client.config_.transport == Transport::http && (response.status == 404 || response.status == 405)))
        return false;
    if (response.rpc_error)
        throw McpError{McpError::Kind::handshake, client.config_.name + ": " + rpc_text(method, response.error)};
    if (!response.ok) {
        if (response.status >= 400) throw_http_status(client.config_.name, method, response.status);
        throw McpError{McpError::Kind::handshake,
                       std::format("{}: {} returned no result", client.config_.name, method)};
    }
    const json supported = member_or(response.result, "supportedVersions", json::array());
    const auto selected = std::ranges::find_if(kRevisions, [&](const Revision& candidate) {
        return candidate.dialect == revision().dialect && detail::supports_protocol(supported, candidate.version);
    });
    if (selected == std::end(kRevisions))
        throw McpError{McpError::Kind::handshake,
                       std::format("{}: server must support MCP {}; advertised versions: {}", client.config_.name,
                                   revision().version, supported.dump())};
    revision_ = selected;
    return true;
}

bool Client::Impl::SessionDialect::handshake(Impl& client, std::stop_token stop) {
    const auto timeout = client.config_.transport == Transport::http
                             ? client.opt_.connect_timeout : client.opt_.probe_timeout;
    json result;
    try {
        result = client.request(revision().handshake_method, {{"protocolVersion", revision().version},
                                                             {"capabilities", json::object()},
                                                             {"clientInfo", detail::client_info()}}, timeout, stop);
    } catch (const McpError& error) {
        if (error.kind() == McpError::Kind::rpc || error.kind() == McpError::Kind::protocol)
            throw McpError{McpError::Kind::handshake, error.what()};
        throw;
    }
    const std::string version = string_or(result, "protocolVersion");
    const auto selected = std::ranges::find_if(kRevisions, [&](const Revision& candidate) {
        return candidate.dialect == revision().dialect && candidate.version == version;
    });
    if (selected == std::end(kRevisions))
        throw McpError{McpError::Kind::handshake,
                       std::format("{}: {} returned unsupported MCP protocolVersion: {}", client.config_.name,
                                   revision().handshake_method, member_or(result, "protocolVersion", nullptr).dump())};
    revision_ = selected;
    (void)client.request("notifications/initialized", json::object(), timeout, stop, true);
    return true;
}

void Client::Impl::negotiate(std::stop_token stop) {
    constexpr DialectKind preference[] = {DialectKind::stateless, DialectKind::session};
    for (const auto kind : preference) {
        const auto preferred = std::ranges::find(kRevisions, kind, &Revision::dialect);
        switch (kind) {
        case DialectKind::stateless: dialect_ = std::make_unique<StatelessDialect>(*preferred); break;
        case DialectKind::session: dialect_ = std::make_unique<SessionDialect>(*preferred); break;
        }
        if (!dialect_->handshake(*this, stop)) continue;
        protocol_version_ = dialect_->revision().version;
        base::logger("mcp")->info("{}: 已连接，协议 {}", config_.name, protocol_version_);
        return;
    }
}

void Client::Impl::fetch_tools(milliseconds timeout, std::stop_token stop) {
    std::vector<Tool> tools;
    std::map<std::string, std::vector<detail::HeaderParam>> header_params;
    std::set<std::string> qualified_names;
    std::optional<std::string> cursor;
    std::string last_cursor;
    int pages = 0;
    while (true) {
        json params = json::object();
        if (cursor.has_value()) params["cursor"] = *cursor;
        const json result = request("tools/list", std::move(params), timeout, stop);
        const auto list = result.find("tools");
        if (!result.is_object() || list == result.end() || !list->is_array())
            throw McpError{McpError::Kind::protocol, config_.name + ": tools/list is missing the tools array"};
        for (const auto& item : *list) {
            if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) {
                base::logger("mcp")->warn("{}: 忽略没有名字的工具", config_.name);
                continue;
            }
            Tool tool;
            tool.server = config_.name;
            tool.name = item["name"].get<std::string>();
            tool.qualified_name = detail::qualified_name(tool.server, tool.name);
            if (tool.qualified_name.size() > kMaxQualifiedToolName) {
                base::logger("mcp")->warn("{}: 工具 {} 清理后的名字 {} 超过 {} 字符，已跳过", config_.name, tool.name,
                                tool.qualified_name, kMaxQualifiedToolName);
                continue;
            }
            tool.description = string_or(item, "description");
            if (const auto schema = item.find("inputSchema");
                schema != item.end() && schema->is_object())
                tool.input_schema = *schema;
            else
                tool.input_schema = json::object();
            std::vector<detail::HeaderParam> params;
            if (config_.transport == Transport::http && dialect_->revision().routing_headers) {
                std::string reason;
                if (!detail::collect_header_params(tool.input_schema, params, reason)) {
                    base::logger("mcp")->warn("{}: 剔除工具 {}：{}", config_.name, tool.name, reason);
                    continue;
                }
            }
            // 放在 x-mcp-header 校验之后：被剔除的工具不占名字，同名的合法工具还能留下。
            if (!qualified_names.insert(tool.qualified_name).second) {
                base::logger("mcp")->warn("{}: 工具名清理后与前面的工具重复（{}），已跳过", config_.name,
                                tool.qualified_name);
                continue;
            }
            if (!params.empty()) header_params.emplace(tool.name, std::move(params));
            tools.push_back(std::move(tool));
        }
        const auto next = result.find("nextCursor");
        if (next == result.end() || !next->is_string() || next->get<std::string>().empty()) break;
        cursor = next->get<std::string>();
        if (*cursor == last_cursor) {
            base::logger("mcp")->warn("{}: nextCursor 没有变化，停止分页", config_.name);
            break;
        }
        last_cursor = *cursor;
        if (++pages >= 100) {
            base::logger("mcp")->warn("{}: 分页超过 100 页，停止", config_.name);
            break;
        }
    }
    base::logger("mcp")->info("{}: 发现 {} 个工具", config_.name, tools.size());
    std::lock_guard lock(data_mu_);
    tools_ = std::move(tools);
    header_params_ = std::move(header_params);
}

// ---- Client ----

Client::Client() = default;
Client::~Client() = default;

std::unique_ptr<Client> Client::connect(const ServerConfig& config, const Options& opt, std::stop_token stop) {
    auto client = std::unique_ptr<Client>(new Client());
    client->impl_ = std::make_unique<Impl>(config, opt);
    try {
        client->impl_->start();
        if (stop.stop_requested())
            throw McpError{McpError::Kind::cancelled, config.name + ": connection interrupted"};
        client->impl_->negotiate(stop);
        client->impl_->fetch_tools(opt.connect_timeout, stop);
    } catch (...) {
        client.reset(); // 走 Impl::shutdown：结束子进程、唤醒等待者
        throw;
    }
    return client;
}

const std::vector<Tool>& Client::tools() const { return impl_->tools_; }

Transport Client::transport() const { return impl_->config_.transport; }

const std::string& Client::protocol_version() const { return impl_->protocol_version_; }

void Client::shutdown() { impl_->shutdown(); }

CallResult Client::call(std::string_view tool, const json& args, milliseconds timeout, std::stop_token stop) {
    std::string name;
    {
        std::lock_guard lock(impl_->data_mu_);
        const auto it = std::ranges::find_if(impl_->tools_, [tool](const Tool& candidate) {
            return candidate.qualified_name == tool;
        });
        if (it == impl_->tools_.end())
            throw McpError{McpError::Kind::protocol,
                           std::format("{}: unknown tool {}", impl_->config_.name, tool)};
        name = it->name;
    }
    json params = {{"name", name}, {"arguments", args.is_object() ? args : json::object()}};
    const json result = impl_->request("tools/call", std::move(params), timeout, stop);
    if (string_or(result, "resultType", "complete") == "input_required")
        throw McpError{McpError::Kind::protocol,
                       impl_->config_.name + ": server requested input (input_required), which this version does not support"};
    CallResult out;
    if (const auto content = result.find("content");
        content != result.end() && content->is_array()) {
        out.content.reserve(content->size());
        for (const auto& block : *content) out.content.push_back(block);
    }
    if (const auto structured = result.find("structuredContent"); structured != result.end())
        out.structured = *structured;
    out.is_error = bool_or(result, "isError");
    return out;
}

} // namespace dagent::mcp
