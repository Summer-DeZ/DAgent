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
};

} // namespace

struct Client::Impl {
    Impl(ServerConfig config, Options opt) : config_(std::move(config)), opt_(std::move(opt)) {}
    ~Impl() { shutdown(); }

    // ---- 生命周期
    void start();
    void shutdown();
    void handshake(std::stop_token stop);
    void fetch_tools(milliseconds timeout, std::stop_token stop);

    // ---- 消息收发
    Response exchange(std::string_view method, json params, milliseconds timeout, std::stop_token stop);
    Response stdio_exchange(std::int64_t id, std::string_view method, json params, milliseconds timeout,
                            std::stop_token stop);
    Response http_exchange(std::int64_t id, std::string_view method, json params, milliseconds timeout,
                           std::stop_token stop);
    json request(std::string_view method, json params, milliseconds timeout, std::stop_token stop);
    void send_cancel(std::int64_t id, std::string_view reason);
    void stdio_send(std::string_view line);
    void on_line(std::string_view line);
    void on_exit(std::optional<int> code, std::optional<int> signal);

    json build_params(json params) const;
    net::Headers build_headers(std::string_view method, const json& params) const;

    ServerConfig config_;
    Options opt_;
    bool http_ = false;

    std::unique_ptr<exec::Child> child_;

    std::atomic<std::int64_t> next_id_{1};
    std::mutex mu_;
    std::condition_variable_any cv_;
    std::map<std::int64_t, std::shared_ptr<Pending>> pending_;
    bool closing_ = false;
    bool disconnected_ = false;
    std::string disconnect_reason_;

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
    if (config_.command.empty())
        throw McpError{McpError::Kind::spawn, config_.name + ": stdio server is missing command"};
    exec::Command cmd;
    cmd.argv = config_.command;
    cmd.cwd = config_.cwd;
    cmd.inherit_env = config_.environment == "project";
    cmd.env_set = config_.env;
    try {
        auto process = opt_.process;
        if (config_.environment == "project") process.environment.clear();
        child_ = exec::Child::spawn(cmd, process);
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
    log_mcp()->debug("{}: 忽略主动消息 {}", config_.name, string_or(message, "method"));
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

// ---- 请求 ----

json Client::Impl::build_params(json params) const {
    if (!params.is_object()) params = json::object();
    params["_meta"] = detail::modern_meta();
    return params;
}

net::Headers Client::Impl::build_headers(std::string_view method, const json& params) const {
    net::Headers headers = config_.headers;
    set_header(headers, "Accept", "application/json, text/event-stream");
    set_header(headers, "Content-Type", "application/json");
    set_header(headers, "MCP-Protocol-Version", std::string(detail::kModernVersion));
    set_header(headers, "Mcp-Method", std::string(method));
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
    return headers;
}

Response Client::Impl::exchange(std::string_view method, json params, milliseconds timeout,
                                std::stop_token stop) {
    const std::int64_t id = next_id_.fetch_add(1);
    if (http_) return http_exchange(id, method, std::move(params), timeout, stop);
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
                                     milliseconds timeout, std::stop_token stop) {
    const json message = detail::make_request(id, method, build_params(std::move(params)));
    net::HttpOptions options = opt_.http;
    options.timeout = std::chrono::seconds{0}; // 总时长交给下面自己的计时器
    // 每次调用独占一个 HttpClient：并发调用互不排队，取消和超时立刻生效（代价是连接不复用）。
    net::HttpClient client(options);
    net::HttpRequest req{.method = "POST", .url = config_.url, .headers = {}, .body = message.dump()};
    req.headers = build_headers(method, message["params"]);

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
        if (!msg.contains("method") && msg_id != msg.end() && msg_id->is_number_integer() &&
            msg_id->get<std::int64_t>() == id) {
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
            cancel.get_token());
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
        if (got) {
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
    Response response = exchange(method, std::move(params), timeout, stop);
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

void Client::Impl::send_cancel(std::int64_t id, std::string_view reason) {
    if (http_) return; // HTTP：断开响应流本身就是取消信号
    try {
        const json message = detail::make_notification(
            "notifications/cancelled", {{"requestId", id}, {"reason", std::string(reason)}});
        stdio_send(message.dump());
    } catch (const std::exception& e) {
        log_mcp()->warn("{}: 发送 notifications/cancelled 失败：{}", config_.name, e.what());
    }
}

// ---- 协议发现 ----

void Client::Impl::handshake(std::stop_token stop) {
    const Response response = exchange("server/discover", json::object(),
                                       http_ ? opt_.connect_timeout : opt_.probe_timeout, stop);
    if (response.status == 401 || response.status == 403 || response.status >= 500)
        throw_http_status(config_.name, "server/discover", response.status, response.error);
    if (response.rpc_error)
        throw McpError{McpError::Kind::handshake, config_.name + ": " + rpc_text("server/discover", response.error)};
    if (!response.ok) {
        if (response.status >= 400) throw_http_status(config_.name, "server/discover", response.status);
        throw McpError{McpError::Kind::handshake, config_.name + ": server/discover returned no result"};
    }
    const json supported = member_or(response.result, "supportedVersions", json::array());
    if (!detail::supports_protocol(supported))
        throw McpError{McpError::Kind::handshake,
                       std::format("{}: server must support MCP {}; advertised versions: {}", config_.name,
                                   detail::kModernVersion, supported.dump())};
    log_mcp()->info("{}: 已连接，协议 {}", config_.name, detail::kModernVersion);
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
