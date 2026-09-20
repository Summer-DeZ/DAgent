#include "mcp/client.hpp"
#include "mcp/detail.hpp"

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
#include "exec/child.hpp"
#include "net/sse.hpp"

namespace dagent::mcp {
namespace {

using json = nlohmann::json;
using std::chrono::milliseconds;

constexpr int kMethodNotFound = -32601;
constexpr int kHeaderMismatch = -32020;
constexpr int kMissingClientCapability = -32021;
constexpr int kUnsupportedProtocolVersion = -32022;
constexpr milliseconds kNotificationTimeout{15000};
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

/// @brief 等待中的 stdio 请求。读取线程填结果，调用线程取走并从 id 表删除。
struct Pending {
    bool done = false;
    bool disconnected = false;
    std::string error_text;
    json result; ///< 成功时的 result；error 非 null 时无效
    json error;  ///< JSON-RPC error 对象
};

/// @brief 一次请求的原始结果；超时/取消/断连以异常表达，业务错误放在这里。
struct Response {
    bool ok = false;
    json result;
    bool rpc_error = false;
    json error;
    long status = 0;          ///< 仅 HTTP
    std::string used_session; ///< 请求发出时带的 Mcp-Session-Id（HTTP 经典）
    std::string session_id;   ///< 响应头里的 Mcp-Session-Id（只有 initialize 会带）
};

} // namespace

struct Client::Impl {
    Impl(ServerConfig config, Options opt) : config_(std::move(config)), opt_(std::move(opt)) {}
    ~Impl() { shutdown(); }

    // ---- 生命周期
    void start();
    void shutdown();
    void handshake(std::stop_token stop);
    bool stdio_probe_modern(std::stop_token stop);
    bool http_probe_modern(std::stop_token stop);
    bool select_modern(const json& supported);
    Response initialize_exchange(std::stop_token stop);
    void legacy_handshake(std::stop_token stop);
    void recover_session(const std::string& used_session, std::stop_token stop);
    void fetch_tools(milliseconds timeout, std::stop_token stop);

    // ---- 消息收发
    Response exchange(std::string_view method, json params, milliseconds timeout, std::stop_token stop,
                      const std::string* session_override = nullptr);
    Response stdio_exchange(std::int64_t id, std::string_view method, json params, milliseconds timeout,
                            std::stop_token stop);
    Response http_exchange(std::int64_t id, std::string_view method, json params, milliseconds timeout,
                           std::stop_token stop, const std::string& session);
    json request(std::string_view method, json params, milliseconds timeout, std::stop_token stop);
    void notify(std::string_view method, json params);
    void send_cancel(std::int64_t id, std::string_view reason);
    void stdio_send(std::string_view line);
    void http_post(const json& message, milliseconds timeout, const std::string* session_override = nullptr);
    void handle_incoming(const json& message);
    void on_line(std::string_view line);
    void on_exit(std::optional<int> code, std::optional<int> signal);

    json build_params(json params) const;
    net::Headers build_headers(std::string_view method, const json& params, bool modern,
                               const std::string& session) const;

    ServerConfig config_;
    Options opt_;
    bool http_ = false;

    bool modern_ = true; ///< 握手前按现代协议探测；探测失败后置 false
    std::string version_;

    std::unique_ptr<exec::Child> child_;

    std::atomic<std::int64_t> next_id_{1};
    std::mutex mu_;
    std::condition_variable_any cv_;
    std::map<std::int64_t, std::shared_ptr<Pending>> pending_;
    bool closing_ = false;
    bool disconnected_ = false;
    std::string disconnect_reason_;

    mutable std::mutex session_mu_; ///< 经典 HTTP 的 Mcp-Session-Id，HTTP 调用并发时保护
    std::string session_id_;
    std::mutex reinit_mu_; ///< 会话失效后只让一个线程重新 initialize，其他线程用换好的会话重试

    mutable std::mutex data_mu_;
    std::vector<Tool> tools_;
    std::map<std::string, std::vector<detail::HeaderParam>> header_params_;
    std::function<void()> tools_changed_;

    std::string session_id() const {
        std::lock_guard lock(session_mu_);
        return session_id_;
    }
    void set_session_id(std::string id) {
        std::lock_guard lock(session_mu_);
        session_id_ = std::move(id);
    }
};

void Client::Impl::start() {
    if (config_.name.empty()) throw McpError{McpError::Kind::handshake, "server name is empty"};
    if (config_.transport == Transport::http) {
        if (config_.url.empty())
            throw McpError{McpError::Kind::handshake, config_.name + ": HTTP server is missing url"};
        http_ = true;
        return;
    }
    if (config_.command.empty())
        throw McpError{McpError::Kind::spawn, config_.name + ": stdio server is missing command"};
    exec::Command cmd;
    cmd.argv = config_.command;
    cmd.env_set = config_.env;
    try {
        child_ = exec::Child::spawn(cmd, opt_.process);
    } catch (const exec::ExecError& e) {
        throw McpError{McpError::Kind::spawn, config_.name + ": " + e.what()};
    }
    child_->on_line([this](std::string_view line) { on_line(line); });
    child_->on_exit([this](std::optional<int> code, std::optional<int> signal) { on_exit(code, signal); });
}

void Client::Impl::shutdown() {
    {
        std::lock_guard lock(mu_);
        if (closing_) return;
        closing_ = true;
        disconnected_ = true;
        for (auto& [id, pending] : pending_) {
            if (pending->done) continue;
            pending->done = true;
            pending->disconnected = true;
            pending->error_text = config_.name + ": client is closed";
        }
    }
    cv_.notify_all();
    if (child_) child_->terminate(); // 等读取线程退出，之后不会再回调
    child_.reset();
}

// ---- stdio 读写 ----

void Client::Impl::stdio_send(std::string_view line) {
    if (!child_) return;
    std::string data(line);
    data.push_back('\n');
    child_->write(data);
}

void Client::Impl::on_line(std::string_view line) {
    json message = json::parse(line, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        log_mcp()->warn("{}: 忽略 server stdout 上的非 JSON 行（{} 字节）", config_.name, line.size());
        return;
    }
    if (!message.contains("method")) {
        const auto id = message.find("id");
        if (id == message.end() || !id->is_number_integer()) {
            log_mcp()->debug("{}: 忽略无法匹配的消息", config_.name);
            return;
        }
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard lock(mu_);
            const auto it = pending_.find(id->get<std::int64_t>());
            if (it == pending_.end()) {
                log_mcp()->debug("{}: 忽略已结束请求 {} 的迟到响应", config_.name, id->get<std::int64_t>());
                return;
            }
            pending = it->second;
            if (message.contains("error")) pending->error = message["error"];
            else pending->result = message.contains("result") ? std::move(message["result"]) : json::object();
            pending->done = true;
        }
        cv_.notify_all();
        return;
    }
    handle_incoming(message);
}

void Client::Impl::on_exit(std::optional<int> code, std::optional<int> signal) {
    std::string reason;
    if (signal.has_value()) reason = std::format("server terminated by signal {}", *signal);
    else reason = std::format("server exited (code={})", code.value_or(-1));
    {
        std::lock_guard lock(mu_);
        disconnected_ = true;
        disconnect_reason_ = config_.name + ": " + reason;
        for (auto& [id, pending] : pending_) {
            if (pending->done) continue;
            pending->done = true;
            pending->disconnected = true;
            pending->error_text = disconnect_reason_;
        }
    }
    cv_.notify_all();
    log_mcp()->warn("{}", disconnect_reason_);
}

// ---- 消息分发 ----

void Client::Impl::handle_incoming(const json& message) {
    const std::string method = string_or(message, "method");
    if (method.empty()) {
        log_mcp()->debug("{}: 忽略未知消息", config_.name);
        return;
    }
    const auto id = message.find("id");
    if (id != message.end()) { // server 主动发来的请求
        if (http_ && modern_) {
            log_mcp()->warn("{}: 现代协议下 server 不应发起请求，忽略 {}", config_.name, method);
            return;
        }
        try {
            if (method == "ping") {
                const json reply = detail::make_response(*id, json::object());
                if (http_) http_post(reply, kNotificationTimeout);
                else stdio_send(reply.dump());
            } else {
                const json reply =
                    detail::make_error_response(*id, kMethodNotFound, "Method not found: " + method);
                if (http_) http_post(reply, kNotificationTimeout);
                else stdio_send(reply.dump());
            }
        } catch (const std::exception& e) {
            log_mcp()->warn("{}: 回复 server 请求 {} 失败：{}", config_.name, method, e.what());
        }
        return;
    }
    if (method == "notifications/tools/list_changed") {
        std::function<void()> callback;
        {
            std::lock_guard lock(data_mu_);
            callback = tools_changed_;
        }
        if (!callback) return;
        try {
            callback();
        } catch (const std::exception& e) {
            log_mcp()->error("{}: tools/list_changed 回调抛出异常：{}", config_.name, e.what());
        }
        return;
    }
    log_mcp()->debug("{}: 忽略通知 {}", config_.name, method);
}

// ---- 请求 ----

json Client::Impl::build_params(json params) const {
    if (!modern_) return params;
    if (!params.is_object()) params = json::object();
    params["_meta"] = detail::modern_meta();
    return params;
}

net::Headers Client::Impl::build_headers(std::string_view method, const json& params, bool modern,
                                         const std::string& session) const {
    net::Headers headers = config_.headers;
    set_header(headers, "Accept", "application/json, text/event-stream");
    set_header(headers, "Content-Type", "application/json");
    if (modern) {
        set_header(headers, "MCP-Protocol-Version", std::string(detail::kModernVersion));
        if (!method.empty()) set_header(headers, "Mcp-Method", std::string(method));
        if (method == "tools/call" && params.is_object()) {
            const std::string name = string_or(params, "name");
            set_header(headers, "Mcp-Name", detail::encode_header_value(name));
            std::lock_guard lock(data_mu_);
            const auto it = header_params_.find(name);
            if (it != header_params_.end()) {
                const json arguments = params.contains("arguments") ? params["arguments"] : json::object();
                for (auto& [header, value] : detail::header_param_values(it->second, arguments))
                    set_header(headers, header, std::move(value));
            }
        }
    } else {
        if (!session.empty()) set_header(headers, "Mcp-Session-Id", session);
        // 2025-06-18 起要求带着协商结果；更早的版本不认识这个头。
        if (version_ >= "2025-06-18") set_header(headers, "MCP-Protocol-Version", version_);
    }
    return headers;
}

Response Client::Impl::exchange(std::string_view method, json params, milliseconds timeout,
                                std::stop_token stop, const std::string* session_override) {
    const std::int64_t id = next_id_.fetch_add(1);
    if (http_) {
        const std::string session = session_override    ? *session_override
                                    : modern_           ? std::string{}
                                                        : session_id();
        return http_exchange(id, method, std::move(params), timeout, stop, session);
    }
    return stdio_exchange(id, method, std::move(params), timeout, stop);
}

Response Client::Impl::stdio_exchange(std::int64_t id, std::string_view method, json params,
                                      milliseconds timeout, std::stop_token stop) {
    std::string line = detail::make_request(id, method, build_params(std::move(params))).dump();
    auto pending = std::make_shared<Pending>();
    {
        std::lock_guard lock(mu_);
        if (closing_)
            throw McpError{McpError::Kind::disconnected, config_.name + ": client is closed"};
        if (disconnected_) throw McpError{McpError::Kind::disconnected, disconnect_reason_};
        pending_[id] = pending;
    }
    stdio_send(line);

    {
        std::unique_lock lock(mu_);
        const auto done = [&pending] { return pending->done; };
        if (timeout.count() > 0) (void)cv_.wait_for(lock, stop, timeout, done);
        else (void)cv_.wait(lock, stop, done);
        if (!pending->done) {
            pending_.erase(id);
            lock.unlock();
            if (stop.stop_requested()) {
                send_cancel(id, "cancelled by caller");
                throw McpError{McpError::Kind::cancelled,
                               std::format("{}: {} interrupted", config_.name, method)};
            }
            send_cancel(id, "timeout");
            throw McpError{McpError::Kind::timeout,
                           std::format("{}: {} timed out ({}ms)", config_.name, method, timeout.count())};
        }
        pending_.erase(id);
    }
    if (pending->disconnected) throw McpError{McpError::Kind::disconnected, pending->error_text};
    Response out;
    if (!pending->error.is_null()) {
        out.rpc_error = true;
        out.error = std::move(pending->error);
    } else {
        out.ok = true;
        out.result = std::move(pending->result);
    }
    return out;
}

Response Client::Impl::http_exchange(std::int64_t id, std::string_view method, json params,
                                     milliseconds timeout, std::stop_token stop,
                                     const std::string& session) {
    const json message = detail::make_request(id, method, build_params(std::move(params)));
    net::HttpOptions options = opt_.http;
    options.timeout = std::chrono::seconds{0}; // 总时长交给下面自己的计时器
    // 每次调用独占一个 HttpClient：并发调用互不排队，取消和超时立刻生效（代价是连接不复用）。
    net::HttpClient client(options);
    net::HttpRequest req{.method = "POST", .url = config_.url, .headers = {}, .body = message.dump()};
    req.headers = build_headers(method, message["params"], modern_, session);

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
    out.used_session = session;
    bool got = false;
    json response_message;
    net::SseParser sse;
    std::string body;
    bool sse_seen = false;   // SSE 流只需要逐事件解析，不再留整包
    bool body_overflow = false;
    // initialize 的响应头里有 Mcp-Session-Id；提前掐掉 SSE 就拿不到响应头了，所以它要读完流。
    const bool need_headers = method == "initialize";

    const auto take = [&](const json& msg) {
        const auto msg_id = msg.find("id");
        if (!msg.contains("method") && msg_id != msg.end() && msg_id->is_number_integer() &&
            msg_id->get<std::int64_t>() == id) {
            response_message = msg;
            got = true;
            if (!need_headers) cancel.request_stop(); // 拿到响应就不用等 server 关闭流了
            return;
        }
        handle_incoming(msg);
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
            cancel.get_token());
        out.status = resp.status;
        // 会话头只信 initialize 的响应；其他响应里的忽略，避免并发时新旧会话互相覆盖。
        if (const auto sid = resp.header("mcp-session-id"); sid && !sid->empty())
            out.session_id = std::string(*sid);
        if (resp.status < 200 || resp.status >= 300) {
            const json msg = json::parse(resp.body, nullptr, false);
            if (!msg.is_discarded() && msg.is_object()) {
                if (msg.contains("error")) {
                    out.rpc_error = true;
                    out.error = msg["error"];
                } else if (msg.contains("method")) {
                    handle_incoming(msg);
                }
            }
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
            if (response_message.contains("error")) {
                out.rpc_error = true;
                out.error = std::move(response_message["error"]);
            } else {
                out.ok = true;
                out.result = response_message.contains("result") ? std::move(response_message["result"])
                                                                 : json::object();
            }
        }
        return out;
    } catch (const net::HttpError& e) {
        if (got && !need_headers) {
            if (response_message.contains("error")) {
                out.rpc_error = true;
                out.error = response_message["error"];
            } else {
                out.ok = true;
                out.result = response_message.contains("result") ? response_message["result"] : json::object();
            }
            return out;
        }
        if (stop.stop_requested()) {
            send_cancel(id, "cancelled by caller");
            throw McpError{McpError::Kind::cancelled, std::format("{}: {} interrupted", config_.name, method)};
        }
        // initialize 要读完流才拿得到响应头：响应到了但流没正常结束（server 不关 SSE 流而超时，或连接
        // 中断），就拿不到 Mcp-Session-Id，不能当成功，否则之后每个请求都会因为缺会话失败。
        if (got)
            throw McpError{McpError::Kind::handshake,
                           std::format("{}: {} response stream did not finish normally; Mcp-Session-Id unavailable ({})", config_.name,
                                       method, e.what())};
        if (deadline_hit) {
            send_cancel(id, "timeout");
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

json Client::Impl::request(std::string_view method, json params, milliseconds timeout, std::stop_token stop) {
    Response response = exchange(method, params, timeout, stop);
    // 经典 HTTP 的会话过期：server 重启后会拒绝旧 session（404），重新 initialize 再试一次。
    if (http_ && !modern_ && response.status == 404 && !response.used_session.empty()) {
        log_mcp()->warn("{}: 会话已失效，重新 initialize 后重试 {}", config_.name, method);
        recover_session(response.used_session, stop);
        response = exchange(method, std::move(params), timeout, stop);
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

void Client::Impl::notify(std::string_view method, json params) {
    const json message = detail::make_notification(method, params);
    if (http_) {
        http_post(message, kNotificationTimeout);
        return;
    }
    stdio_send(message.dump());
}

void Client::Impl::http_post(const json& message, milliseconds timeout, const std::string* session_override) {
    net::HttpOptions options = opt_.http;
    if (options.timeout.count() == 0 || options.timeout > timeout)
        options.timeout = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    if (options.connect_timeout.count() == 0 || options.connect_timeout > timeout)
        options.connect_timeout = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    net::HttpClient client(options);
    net::HttpRequest req{.method = "POST", .url = config_.url, .headers = {}, .body = message.dump()};
    req.headers = build_headers("", json::object(), modern_, session_override ? *session_override : session_id());
    net::HttpResponse resp;
    try {
        resp = client.send(req);
    } catch (const net::HttpError& e) {
        const auto kind = e.kind() == net::HttpError::Kind::timeout ? McpError::Kind::timeout
                                                                    : McpError::Kind::disconnected;
        throw McpError{kind, std::format("{}: POST notification failed: {}", config_.name, e.what())};
    }
    if (resp.status < 200 || resp.status >= 300)
        throw_http_status(config_.name, "POST notification", resp.status);
}

void Client::Impl::send_cancel(std::int64_t id, std::string_view reason) {
    if (http_ && modern_) return; // 现代 HTTP：断开响应流本身就是取消信号
    try {
        const json message = detail::make_notification(
            "notifications/cancelled", {{"requestId", id}, {"reason", std::string(reason)}});
        if (http_) http_post(message, milliseconds{5000});
        else stdio_send(message.dump());
    } catch (const std::exception& e) {
        log_mcp()->warn("{}: 发送 notifications/cancelled 失败：{}", config_.name, e.what());
    }
}

// ---- 握手 ----

bool Client::Impl::select_modern(const json& supported) {
    const std::string version = detail::pick_modern_version(supported);
    if (!version.empty()) {
        version_ = version;
        return true;
    }
    // discover 是现代方法，但 server 也可以只提供经典版本：按经典协议握手。
    if (supported.is_array()) {
        for (const auto& item : supported)
            if (item.is_string() && detail::is_known_legacy_version(item.get<std::string>())) return false;
    }
    throw McpError{McpError::Kind::handshake,
                   std::format("{}: none of the server protocol versions {} is supported by this client", config_.name,
                               supported.dump())};
}

bool Client::Impl::stdio_probe_modern(std::stop_token stop) {
    Response response;
    try {
        response = stdio_exchange(next_id_.fetch_add(1), "server/discover", json::object(), opt_.probe_timeout,
                                  stop);
    } catch (const McpError& e) {
        if (e.kind() == McpError::Kind::timeout) {
            log_mcp()->debug("{}: server/discover 无响应，按经典协议握手", config_.name);
            return false;
        }
        throw;
    }
    if (response.ok) return select_modern(member_or(response.result, "supportedVersions", json::array()));
    if (response.rpc_error && int_or(response.error, "code") == kUnsupportedProtocolVersion)
        return select_modern(member_or(member_or(response.error, "data", json::object()), "supported", json::array()));
    return false; // 其他错误（-32601 等）说明是经典 server
}

bool Client::Impl::http_probe_modern(std::stop_token stop) {
    const Response response = exchange("server/discover", json::object(), opt_.connect_timeout, stop);
    if (response.status == 401 || response.status == 403 || response.status >= 500)
        throw_http_status(config_.name, "server/discover", response.status, response.error);
    if (response.ok) return select_modern(member_or(response.result, "supportedVersions", json::array()));
    if (response.rpc_error) {
        const int code = int_or(response.error, "code");
        const json supported = member_or(member_or(response.error, "data", json::object()), "supported",
                                         json::array());
        if (code == kUnsupportedProtocolVersion) return select_modern(supported);
        if (code == kHeaderMismatch || code == kMissingClientCapability)
            throw McpError{McpError::Kind::handshake,
                           config_.name + ": " + rpc_text("server/discover", response.error)};
        if (code == kMethodNotFound && response.status == 404) {
            version_ = std::string(detail::kModernVersion); // 现代 server 但没实现 discover
            return true;
        }
        return false;
    }
    // 经典 server 在初始化前对未知请求通常回 400/404/405；其他状态码按真实错误处理。
    if (response.status == 400 || response.status == 404 || response.status == 405) return false;
    if (response.status >= 400) throw_http_status(config_.name, "server/discover", response.status);
    throw McpError{McpError::Kind::protocol,
                   std::format("{}: unrecognized server/discover response (HTTP {})", config_.name, response.status)};
}

// 发送 initialize。新会话在 response.session_id 里，由调用方决定什么时候装上；不碰 version_ / modern_。
Response Client::Impl::initialize_exchange(std::stop_token stop) {
    const json params = {{"protocolVersion", std::string(detail::kPreferredLegacy)},
                         {"capabilities", json::object()},
                         {"clientInfo", detail::client_info()}};
    const std::string no_session; // initialize 不能带旧会话，否则 server 会拿它当续期请求拒掉
    return exchange("initialize", params, opt_.connect_timeout, stop, &no_session);
}

void Client::Impl::legacy_handshake(std::stop_token stop) {
    Response response = initialize_exchange(stop);
    if (response.status == 401 || response.status == 403 || response.status >= 500)
        throw_http_status(config_.name, "initialize", response.status, response.error);
    if (response.rpc_error) {
        const json supported = member_or(member_or(response.error, "data", json::object()), "supported",
                                         json::array());
        const std::string modern = detail::pick_modern_version(supported);
        // 冷启动慢的现代 server 会先让 server/discover 探测超时，但 initialize 会被它用
        // UnsupportedProtocolVersionError 明确拒绝：这时改走现代协议，不再当它是经典 server。
        if (int_or(response.error, "code") == kUnsupportedProtocolVersion && !modern.empty()) {
            modern_ = true;
            version_ = modern;
            log_mcp()->info("{}: server 拒绝 initialize，改用现代协议 {}", config_.name, modern);
            return;
        }
        throw McpError{McpError::Kind::rpc, config_.name + ": " + rpc_text("initialize", response.error)};
    }
    if (!response.ok) {
        if (response.status >= 400) throw_http_status(config_.name, "initialize", response.status);
        throw McpError{McpError::Kind::protocol,
                       std::format("{}: initialize response contains neither result nor error", config_.name)};
    }
    const std::string version = string_or(response.result, "protocolVersion");
    if (!detail::is_known_legacy_version(version))
        throw McpError{McpError::Kind::handshake,
                       std::format("{}: negotiated server protocol version is unsupported: '{}'", config_.name, version)};
    version_ = version;
    if (http_) set_session_id(response.session_id); // connect 期间只有这一个线程，先装上再发通知
    try {
        notify("notifications/initialized", json::object());
    } catch (const McpError& e) {
        throw McpError{McpError::Kind::handshake,
                       std::format("{}: failed to send notifications/initialized: {}", config_.name, e.what())};
    }
}

// 会话过期后的恢复。同一时刻只允许一个线程重新 initialize；其他线程如果发现会话已经被
// 换过，直接返回去用新会话重试。这里只重建会话，不改 version_ / modern_（server 换了协议
// 就报错，让核心重新 connect），所以握手之后这两个字段是只读的。
void Client::Impl::recover_session(const std::string& used_session, std::stop_token stop) {
    std::lock_guard lock(reinit_mu_);
    if (session_id() != used_session) return;
    log_mcp()->warn("{}: 会话已失效，重新 initialize", config_.name);
    Response response = initialize_exchange(stop);
    if (response.status == 401 || response.status == 403 || response.status >= 500)
        throw_http_status(config_.name, "initialize", response.status, response.error);
    if (response.rpc_error) {
        if (int_or(response.error, "code") == kUnsupportedProtocolVersion)
            throw McpError{McpError::Kind::handshake,
                           config_.name + ": server no longer supports the classic protocol after restart; reconnect required"};
        throw McpError{McpError::Kind::rpc, config_.name + ": " + rpc_text("initialize", response.error)};
    }
    if (!response.ok) {
        if (response.status >= 400) throw_http_status(config_.name, "initialize", response.status);
        throw McpError{McpError::Kind::protocol,
                       std::format("{}: initialize response contains neither result nor error", config_.name)};
    }
    const std::string version = string_or(response.result, "protocolVersion");
    if (version != version_)
        throw McpError{McpError::Kind::handshake,
                       std::format("{}: negotiated protocol changed after server restart ({} -> {}); reconnect required",
                                   config_.name, version_, version)};
    // 通知显式带新会话发出，成功后才装进共享状态：其他线程不会拿到还没初始化完的会话。
    try {
        http_post(detail::make_notification("notifications/initialized", json::object()), kNotificationTimeout,
                  &response.session_id);
    } catch (const McpError& e) {
        throw McpError{McpError::Kind::handshake,
                       std::format("{}: failed to send notifications/initialized: {}", config_.name, e.what())};
    }
    set_session_id(response.session_id);
}

void Client::Impl::handshake(std::stop_token stop) {
    const bool probed_modern = http_ ? http_probe_modern(stop) : stdio_probe_modern(stop);
    if (probed_modern) {
        modern_ = true;
    } else {
        modern_ = false;
        legacy_handshake(stop); // 里面可能因为 -32022 改判为现代
    }
    log_mcp()->info("{}: 已连接，协议 {}（{}）", config_.name, version_, modern_ ? "现代" : "经典");
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
            if (http_) {
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
        client->impl_->handshake(stop);
        client->impl_->fetch_tools(opt.connect_timeout, stop);
    } catch (...) {
        client.reset(); // 走 Impl::shutdown：结束子进程、唤醒等待者
        throw;
    }
    return client;
}

const std::vector<Tool>& Client::tools() const { return impl_->tools_; }

void Client::refresh_tools(milliseconds timeout, std::stop_token stop) {
    impl_->fetch_tools(timeout, stop);
}

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

void Client::on_tools_changed(std::function<void()> cb) {
    std::lock_guard lock(impl_->data_mu_);
    impl_->tools_changed_ = std::move(cb);
}

const std::string& Client::protocol_version() const { return impl_->version_; }

} // namespace dagent::mcp
