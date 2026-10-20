#include "mcp/client.hpp"
#include "mcp/detail.hpp"
#include "mcp/transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/log.hpp"
#include "net/sse.hpp"

namespace dagent::mcp {
namespace {

using json = nlohmann::json;
using std::chrono::milliseconds;
using detail::Response;

constexpr std::size_t kMaxQualifiedToolName = 64; ///< OpenAI 等协议对函数名的长度限制

std::shared_ptr<spdlog::logger> log_mcp() { return base::logger("mcp"); }

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

// HTTP 已匹配响应的解包：随调用方值类别移动或复制字段，保留已有 HTTP status。
template<class Json>
void unpack_rpc_response(Response& out, Json&& message) {
    if (message.contains("error")) {
        out.rpc_error = true;
        out.error = std::forward_like<Json>(message["error"]);
    } else {
        out.ok = true;
        out.result = message.contains("result") ? std::forward_like<Json>(message["result"])
                                                : json::object();
    }
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
    bool http_ = false;
    std::unique_ptr<Dialect> dialect_;
    std::string protocol_version_; ///< 握手完成后不再改变

    std::atomic<std::int64_t> next_id_{1};
    detail::ConnectionState connection_;
    std::unique_ptr<detail::StdioTransport> stdio_;

    mutable std::mutex data_mu_;
    std::vector<Tool> tools_;
    std::map<std::string, std::vector<detail::HeaderParam>> header_params_;

};

void Client::Impl::start() {
    if (config_.name.empty()) throw McpError{McpError::Kind::handshake, "server name is empty"};
    if (config_.transport == Transport::http) {
        if (config_.url.empty())
            throw McpError{McpError::Kind::handshake, config_.name + ": HTTP server is missing url"};
        http_ = true;
        return;
    }
    stdio_ = std::make_unique<detail::StdioTransport>(config_, opt_, connection_);
    stdio_->start();
}

void Client::Impl::shutdown() {
    if (stdio_) {
        stdio_->shutdown([this] { if (dialect_) dialect_->close(*this); });
        return;
    }
    {
        std::lock_guard lock(connection_.mutex);
        if (connection_.closing) return;
        connection_.closing = true;
        connection_.disconnected = true;
    }
    if (dialect_) dialect_->close(*this);
}

void Client::Impl::close_http_session(const net::Headers& headers) {
    try {
        net::HttpOptions options = opt_.http;
        options.timeout = std::chrono::seconds{5}; // 关闭会话不能无限等待远端
        net::HttpClient client(options);
        const auto response = client.send({.method = "DELETE", .url = config_.url,
                                           .headers = headers, .body = {}});
        if ((response.status < 200 || response.status >= 300) &&
            response.status != 404 && response.status != 405)
            log_mcp()->warn("{}: 关闭 HTTP 会话失败 (HTTP {})", config_.name, response.status);
    } catch (const std::exception& error) {
        log_mcp()->warn("{}: 关闭 HTTP 会话失败：{}", config_.name, error.what());
    }
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
    if (client.http_ && revision().http_session && !session_id_.empty())
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
    if (http_) return http_exchange(id, method, std::move(params), timeout, stop);
    if (notification) return stdio_->notify(method, std::move(params), stop);
    return stdio_->exchange(*id, method, dialect_->decorate_params(std::move(params)), timeout, stop);
}

Response Client::Impl::http_exchange(std::optional<std::int64_t> id, std::string_view method, json params,
                                     milliseconds timeout, std::stop_token stop) {
    const json message = id ? detail::make_request(*id, method, dialect_->decorate_params(std::move(params)))
                            : detail::make_notification(method, std::move(params));
    net::HttpOptions options = opt_.http;
    options.timeout = std::chrono::seconds{0}; // 总时长交给下面自己的计时器
    // 每次调用独占一个 HttpClient：并发调用互不排队，取消和超时立刻生效（代价是连接不复用）。
    net::HttpClient client(options);
    net::HttpRequest req{.method = "POST", .url = config_.url, .headers = {}, .body = message.dump()};
    req.headers = dialect_->headers(*this, method, message["params"]);

    // HTTP：断开响应流本身就是取消信号，不发送 notifications/cancelled。
    std::stop_source cancel;
    std::stop_callback relay(stop, [&cancel] { cancel.request_stop(); });
    std::atomic<bool> deadline_hit{false};
    std::mutex timer_mutex;
    std::condition_variable_any timer_cv;
    std::jthread timer;
    if (timeout.count() > 0) {
        timer = std::jthread([&](std::stop_token token) {
            std::unique_lock wait_lock(timer_mutex);
            if (timer_cv.wait_for(wait_lock, token, timeout, [&] { return token.stop_requested(); })) return;
            deadline_hit = true;
            cancel.request_stop();
        });
    }

    Response out;
    bool got = false;
    json response_message;
    net::SseParser sse;
    std::string body;
    bool sse_seen = false;   // SSE 流只需要逐事件解析，不再留整包
    bool body_overflow = false;

    const auto take = [&](const json& msg) {
        const auto msg_id = msg.find("id");
        if (id && !msg.contains("method") && msg_id != msg.end() && msg_id->is_number_integer() &&
            msg_id->get<std::int64_t>() == *id) {
            response_message = msg;
            got = true;
            cancel.request_stop(); // 拿到响应就不用等 server 关闭流了
            return;
        }
        log_mcp()->debug("{}: 忽略非本次请求的消息", config_.name);
    };

    try {
        const net::HttpResponse resp = client.stream(
            req,
            [&](std::string_view chunk) {
                if (!sse_seen) {
                    if (body.size() + chunk.size() > opt_.http.max_body_bytes) {
                        body_overflow = true;
                        cancel.request_stop(); // 不再继续读这个过大的整包
                    } else {
                        body.append(chunk);
                    }
                }
                sse.feed(chunk, [&](const net::SseEvent& event) {
                    sse_seen = true;
                    body.clear();
                    const json msg = json::parse(event.data, nullptr, false);
                    if (!msg.is_discarded() && msg.is_object()) take(msg);
                });
            },
            cancel.get_token(),
            [&](const net::HttpResponse& headers) {
                out.status = headers.status;
                dialect_->capture_session(method, headers);
            });
        out.status = resp.status;
        if (resp.status < 200 || resp.status >= 300) {
            const json msg = json::parse(resp.body, nullptr, false);
            if (!msg.is_discarded() && msg.is_object()) {
                if (msg.contains("error")) {
                    out.rpc_error = true;
                    out.error = msg["error"];
                }
            }
            return out;
        }
        if (!id) {
            out.ok = true; // 通知没有 JSON-RPC 响应，通常返回 HTTP 202
            return out;
        }
        if (!got) {
            if (body_overflow)
                throw McpError{McpError::Kind::protocol,
                               std::format("{}: {} response size limit reached ({} bytes)", config_.name, method,
                                           opt_.http.max_body_bytes)};
            const json msg = json::parse(body, nullptr, false);
            if (!msg.is_discarded() && msg.is_object()) take(msg);
        }
        if (got) {
            unpack_rpc_response(out, std::move(response_message));
        }
        return out;
    } catch (const net::HttpError& e) {
        if (got) {
            unpack_rpc_response(out, response_message);
            return out;
        }
        if (stop.stop_requested()) {
            throw McpError{McpError::Kind::cancelled, std::format("{}: {} interrupted", config_.name, method)};
        }
        if (deadline_hit) {
            throw McpError{McpError::Kind::timeout,
                           std::format("{}: {} timed out ({}ms)", config_.name, method, timeout.count())};
        }
        if (body_overflow)
            throw McpError{McpError::Kind::protocol,
                           std::format("{}: {} response size limit reached ({} bytes)", config_.name, method,
                                       opt_.http.max_body_bytes)};
        switch (e.kind()) {
        case net::HttpError::Kind::timeout:
            throw McpError{McpError::Kind::timeout, std::format("{}: {} timed out: {}", config_.name, method, e.what())};
        case net::HttpError::Kind::cancelled:
            throw McpError{McpError::Kind::cancelled, std::format("{}: {} interrupted", config_.name, method)};
        case net::HttpError::Kind::too_large:
            throw McpError{McpError::Kind::protocol,
                           std::format("{}: {} response size limit reached: {}", config_.name, method, e.what())};
        case net::HttpError::Kind::connect:
        case net::HttpError::Kind::tls:
        case net::HttpError::Kind::transport:
            throw McpError{McpError::Kind::disconnected,
                           std::format("{}: {} transport failed: {}", config_.name, method, e.what())};
        }
        throw McpError{McpError::Kind::disconnected, e.what()}; // 枚举已经列全，这里到不了
    }
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
                                               client.http_ ? client.opt_.connect_timeout : client.opt_.probe_timeout,
                                               stop);
    if (response.status == 401 || response.status == 403 || response.status >= 500)
        throw_http_status(client.config_.name, method, response.status, response.error);
    if ((response.rpc_error && int_or(response.error, "code") == -32601) ||
        (client.http_ && (response.status == 404 || response.status == 405)))
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
    const auto timeout = client.http_ ? client.opt_.connect_timeout : client.opt_.probe_timeout;
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
        log_mcp()->info("{}: 已连接，协议 {}", config_.name, protocol_version_);
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
                log_mcp()->warn("{}: 忽略没有名字的工具", config_.name);
                continue;
            }
            Tool tool;
            tool.server = config_.name;
            tool.name = item["name"].get<std::string>();
            tool.qualified_name = detail::qualified_name(tool.server, tool.name);
            if (tool.qualified_name.size() > kMaxQualifiedToolName) {
                log_mcp()->warn("{}: 工具 {} 清理后的名字 {} 超过 {} 字符，已跳过", config_.name, tool.name,
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
            if (http_ && dialect_->revision().routing_headers) {
                std::string reason;
                if (!detail::collect_header_params(tool.input_schema, params, reason)) {
                    log_mcp()->warn("{}: 剔除工具 {}：{}", config_.name, tool.name, reason);
                    continue;
                }
            }
            // 放在 x-mcp-header 校验之后：被剔除的工具不占名字，同名的合法工具还能留下。
            if (!qualified_names.insert(tool.qualified_name).second) {
                log_mcp()->warn("{}: 工具名清理后与前面的工具重复（{}），已跳过", config_.name,
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
            log_mcp()->warn("{}: nextCursor 没有变化，停止分页", config_.name);
            break;
        }
        last_cursor = *cursor;
        if (++pages >= 100) {
            log_mcp()->warn("{}: 分页超过 100 页，停止", config_.name);
            break;
        }
    }
    log_mcp()->info("{}: 发现 {} 个工具", config_.name, tools.size());
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

Transport Client::transport() const { return impl_->http_ ? Transport::http : Transport::stdio; }

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
