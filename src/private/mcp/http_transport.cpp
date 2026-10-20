#include "mcp/transport.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <thread>
#include <utility>

#include "base/log.hpp"
#include "net/sse.hpp"

namespace dagent::mcp::detail {
namespace {

using json = nlohmann::json;
using std::chrono::milliseconds;

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

} // namespace

HttpTransport::HttpTransport(const ServerConfig& config, const Options& opt, ConnectionState& connection)
    : config_(config), opt_(opt), connection_(connection) {}

void HttpTransport::start() {
    if (config_.url.empty())
        throw McpError{McpError::Kind::handshake, config_.name + ": HTTP server is missing url"};
}

void HttpTransport::shutdown(const CloseProtocol& close_protocol) {
    {
        std::lock_guard lock(connection_.mutex);
        if (connection_.closing) return;
        connection_.closing = true;
        connection_.disconnected = true;
    }
    close_protocol();
}

void HttpTransport::close_session(const net::Headers& headers) {
    try {
        net::HttpOptions options = opt_.http;
        options.timeout = std::chrono::seconds{5}; // 关闭会话不能无限等待远端
        net::HttpClient client(options);
        const auto response = client.send({.method = "DELETE", .url = config_.url,
                                           .headers = headers, .body = {}});
        if ((response.status < 200 || response.status >= 300) &&
            response.status != 404 && response.status != 405)
            base::logger("mcp")->warn("{}: 关闭 HTTP 会话失败 (HTTP {})", config_.name, response.status);
    } catch (const std::exception& error) {
        base::logger("mcp")->warn("{}: 关闭 HTTP 会话失败：{}", config_.name, error.what());
    }
}

Response HttpTransport::exchange(std::optional<std::int64_t> id, std::string_view method,
                                const json& message, milliseconds timeout, std::stop_token stop,
                                const HeaderFactory& make_headers, const HeaderObserver& observe_headers) {
    net::HttpOptions options = opt_.http;
    options.timeout = std::chrono::seconds{0}; // 总时长交给下面自己的计时器
    // 每次调用独占一个 HttpClient：并发调用互不排队，取消和超时立刻生效（代价是连接不复用）。
    net::HttpClient client(options);
    net::HttpRequest req{.method = "POST", .url = config_.url, .headers = {}, .body = message.dump()};
    req.headers = make_headers();

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
        base::logger("mcp")->debug("{}: 忽略非本次请求的消息", config_.name);
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
                observe_headers(headers);
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

} // namespace dagent::mcp::detail
