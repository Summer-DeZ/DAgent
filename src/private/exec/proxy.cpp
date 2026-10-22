#include "exec/proxy.hpp"

#include "base/log.hpp"
#include "exec/platform.hpp"
#include "exec/process.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <chrono>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dagent::exec {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using boost::system::error_code;

constexpr std::size_t kMaxHeadBytes = 64 << 10;
/// 还有后备地址时单个地址的连接上限；被黑洞的地址不能让命令等到内核的 SYN 超时。
constexpr auto kAttemptTimeout = std::chrono::seconds(5);

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// 缓存与日志使用的目标键；主机名不区分大小写，IPv6 字面量加方括号。
std::string target_key(std::string_view host, int port) {
    const std::string name = lower(host);
    return name.find(':') != std::string::npos ? std::format("[{}]:{}", name, port)
                                               : std::format("{}:{}", name, port);
}

/// `host:port` 或 `[v6]:port`；default_port 为 0 时端口必填。
bool split_authority(std::string_view authority, int default_port, std::string& host, int& port) {
    std::string_view port_text;
    bool has_port = false;
    if (authority.starts_with('[')) {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return false;
        host = authority.substr(1, close - 1);
        const auto rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return false;
            port_text = rest.substr(1);
            has_port = true;
        }
    } else {
        const auto colon = authority.rfind(':');
        host = authority.substr(0, colon);
        if (colon != std::string_view::npos) {
            port_text = authority.substr(colon + 1);
            has_port = true;
        }
    }
    if (host.empty()) return false;
    if (!has_port) {
        port = default_port;
        return port != 0;
    }
    const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    return error == std::errc{} && end == port_text.data() + port_text.size() && port >= 1 && port <= 65535;
}

std::string response(std::string_view status, std::string_view body) {
    return std::format("HTTP/1.1 {}\r\nContent-Type: text/plain\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                       status, body.size(), body);
}

/// 解析后的代理请求：CONNECT 隧道，或已改写为 origin 形式的明文 HTTP 请求头。
struct ProxyRequest {
    bool tunnel = false;
    std::string host;
    int port = 0;
    std::string head; ///< 明文 HTTP 转发给目标的请求头（含结尾空行）
};

bool parse_request(std::string_view head, ProxyRequest& request) {
    const auto line_end = head.find("\r\n");
    const std::string_view line = head.substr(0, line_end);
    const auto first = line.find(' ');
    const auto last = line.rfind(' ');
    if (first == std::string_view::npos || first == last) return false;
    const std::string_view method = line.substr(0, first);
    const std::string_view target = line.substr(first + 1, last - first - 1);
    const std::string_view version = line.substr(last + 1);

    if (method == "CONNECT") {
        request.tunnel = true;
        return split_authority(target, 0, request.host, request.port);
    }
    if (target.size() < 7 || lower(target.substr(0, 7)) != "http://") return false;
    const std::string_view rest = target.substr(7);
    const auto path_start = rest.find_first_of("/?");
    std::string_view authority = rest.substr(0, path_start);
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);
    if (!split_authority(authority, 80, request.host, request.port)) return false;
    std::string path = path_start == std::string_view::npos ? "/" : std::string(rest.substr(path_start));
    if (path.front() == '?') path.insert(path.begin(), '/');

    request.head = std::format("{} {} {}\r\n", method, path, version);
    std::string_view headers = head.substr(line_end + 2);
    while (!headers.empty()) {
        const auto end = headers.find("\r\n");
        const std::string_view header = headers.substr(0, end);
        headers.remove_prefix(end == std::string_view::npos ? headers.size() : end + 2);
        if (header.empty()) break;
        const auto colon = header.find(':');
        if (colon != std::string_view::npos && lower(header.substr(0, colon)) == "proxy-connection") continue;
        request.head.append(header);
        request.head += "\r\n";
    }
    request.head += "\r\n";
    return true;
}

} // namespace

struct NetworkProxy::Impl {
    using Decision = std::function<void(bool allow, const std::string& reason)>;

    struct Pending {
        std::uint64_t id = 0;
        std::vector<Decision> waiters;
        std::unique_ptr<asio::steady_timer> timer;
    };

    struct Session : std::enable_shared_from_this<Session> {
        Impl& proxy;
        tcp::socket client;
        tcp::socket upstream;
        tcp::resolver resolver;
        tcp::resolver::results_type endpoints;
        tcp::resolver::results_type::const_iterator next_endpoint;
        asio::steady_timer attempt_timer;
        asio::streambuf head{kMaxHeadBytes};
        ProxyRequest request;
        std::string pending_write;
        std::array<char, 16 << 10> to_upstream{};
        std::array<char, 16 << 10> to_client{};
        int open_directions = 2;
        bool closed = false;

        Session(Impl& owner, tcp::socket socket)
            : proxy(owner), client(std::move(socket)), upstream(owner.context), resolver(owner.context),
              attempt_timer(owner.context) {}

        void close() {
            error_code ignored;
            closed = true;
            resolver.cancel();
            attempt_timer.cancel();
            client.close(ignored);
            upstream.close(ignored);
        }

        void reply_and_close(std::string text) {
            pending_write = std::move(text);
            asio::async_write(client, asio::buffer(pending_write), [self = shared_from_this()](error_code, std::size_t) {
                error_code ignored;
                self->client.shutdown(tcp::socket::shutdown_send, ignored);
                self->close();
            });
        }

        void read_head() {
            asio::async_read_until(client, head, "\r\n\r\n", [self = shared_from_this()](error_code ec, std::size_t size) {
                if (ec == asio::error::not_found) return self->reply_and_close(response("400 Bad Request", "request head too large\n"));
                if (ec) return self->close();
                self->on_head(size);
            });
        }

        void on_head(std::size_t size) {
            const auto data = head.data();
            const std::string text(asio::buffers_begin(data), asio::buffers_begin(data) + static_cast<std::ptrdiff_t>(size));
            head.consume(size);
            if (!parse_request(text, request))
                return reply_and_close(response("400 Bad Request", "dagent proxy accepts CONNECT and absolute http:// requests\n"));
            proxy.decide(request.host, request.port, [self = shared_from_this()](bool allow, const std::string& reason) {
                if (!allow) {
                    const std::string body = std::format("dagent: network access to {} denied{}{}\n",
                                                         target_key(self->request.host, self->request.port),
                                                         reason.empty() ? "" : ": ", reason);
                    return self->reply_and_close(response("403 Forbidden", body));
                }
                self->connect();
            });
        }

        // 主机名只在放行之后由宿主解析。
        void connect() {
            resolver.async_resolve(request.host, std::to_string(request.port),
                [self = shared_from_this()](error_code ec, const tcp::resolver::results_type& results) {
                    if (self->closed) return;
                    if (ec) return self->reply_and_close(response("502 Bad Gateway", "resolve failed: " + ec.message() + "\n"));
                    self->endpoints = results;
                    self->next_endpoint = self->endpoints.begin();
                    self->try_next(asio::error::host_not_found);
                });
        }

        // 依次尝试解析出的地址；后面还有地址时每次最多等 kAttemptTimeout。
        void try_next(error_code last) {
            if (next_endpoint == endpoints.end())
                return reply_and_close(response("502 Bad Gateway", "connect failed: " + last.message() + "\n"));
            const tcp::endpoint endpoint = (next_endpoint++)->endpoint();
            error_code ignored;
            upstream.close(ignored);
            if (next_endpoint != endpoints.end()) {
                attempt_timer.expires_after(kAttemptTimeout);
                attempt_timer.async_wait([self = shared_from_this()](error_code ec) {
                    error_code close_ignored;
                    if (!ec && !self->closed) self->upstream.close(close_ignored);
                });
            }
            upstream.async_connect(endpoint, [self = shared_from_this()](error_code ec) {
                self->attempt_timer.cancel();
                if (self->closed) return;
                if (ec == asio::error::operation_aborted) ec = asio::error::timed_out;
                if (ec) return self->try_next(ec);
                self->connected();
            });
        }

        void connected() {
            // 请求头之后已读入的字节（请求体或提前发出的 TLS 数据）原样转给目标。
            const auto data = head.data();
            std::string early(asio::buffers_begin(data), asio::buffers_end(data));
            head.consume(head.size());
            if (request.tunnel) {
                pending_write = "HTTP/1.1 200 Connection established\r\n\r\n";
                asio::async_write(client, asio::buffer(pending_write),
                    [self = shared_from_this(), early = std::move(early)](error_code ec, std::size_t) mutable {
                        if (ec) return self->close();
                        self->forward_then_relay(std::move(early));
                    });
            } else {
                forward_then_relay(request.head + early);
            }
        }

        void forward_then_relay(std::string data) {
            if (data.empty()) return relay();
            pending_write = std::move(data);
            asio::async_write(upstream, asio::buffer(pending_write), [self = shared_from_this()](error_code ec, std::size_t) {
                if (ec) return self->close();
                self->relay();
            });
        }

        void relay() {
            pump(client, upstream, to_upstream);
            pump(upstream, client, to_client);
        }

        void pump(tcp::socket& from, tcp::socket& to, std::array<char, 16 << 10>& buffer) {
            from.async_read_some(asio::buffer(buffer), [self = shared_from_this(), &from, &to, &buffer](error_code ec, std::size_t size) {
                if (ec) {
                    // 一端正常结束只半关闭另一端的发送方向，两个方向都结束后关闭连接。
                    error_code ignored;
                    if (ec == asio::error::eof) to.shutdown(tcp::socket::shutdown_send, ignored);
                    else self->close();
                    if (--self->open_directions == 0) self->close();
                    return;
                }
                asio::async_write(to, asio::buffer(buffer.data(), size), [self, &from, &to, &buffer](error_code write_ec, std::size_t) {
                    if (write_ec) {
                        self->close();
                        if (--self->open_directions == 0) self->close();
                        return;
                    }
                    self->pump(from, to, buffer);
                });
            });
        }
    };

    Impl(NetworkGate network_gate, Limits proxy_limits, std::function<void()> cancel)
        : gate(std::move(network_gate)), limits(proxy_limits), on_cancel(std::move(cancel)) {}

    // 只在服务线程上调用；结果经 done 交回服务线程。
    void decide(const std::string& host, int port, Decision done) {
        const std::string key = target_key(host, port);
        if (allowed.contains(key)) return done(true, {});
        if (cancelled || expired) return done(false, "runtime network approval is closed for this command");
        if (const auto it = pending.find(key); it != pending.end()) {
            it->second.waiters.push_back(std::move(done));
            return;
        }
        if (!gate) return done(false, {});
        if (asked >= limits.max_requests) {
            base::logger("exec")->info("network proxy: request budget exhausted, denying {}", key);
            return done(false, "network request budget exhausted");
        }
        ++asked;
        const std::uint64_t id = ++next_id;
        auto& entry = pending[key];
        entry.id = id;
        entry.waiters.push_back(std::move(done));
        if (limits.approval_timeout.count() > 0) {
            entry.timer = std::make_unique<asio::steady_timer>(context, limits.approval_timeout);
            entry.timer->async_wait([this, key, id](error_code ec) {
                if (ec) return;
                // 审批超时视为拒绝，之后不再提问；仍在等待的闸门随 stop_token 释放。
                expired = true;
                gates.request_stop();
                resolve(key, id, NetworkGateResult::deny, "network approval timed out");
            });
        }
        // 闸门可能等待人工审批，不能阻塞 io_context。
        const std::lock_guard lock(deciders_mutex);
        deciders.emplace_back([this, host, port, key, id] {
            std::string reason;
            NetworkGateResult result = NetworkGateResult::deny;
            try {
                result = gate(host, port, reason, gates.get_token());
            } catch (const std::exception& e) {
                base::logger("exec")->warn("network gate failed for {}: {}", key, e.what());
                result = NetworkGateResult::cancel;
            }
            asio::post(context, [this, key, id, result, reason = std::move(reason)] { resolve(key, id, result, reason); });
        });
    }

    void resolve(const std::string& key, std::uint64_t id, NetworkGateResult result, const std::string& reason) {
        const auto it = pending.find(key);
        if (it == pending.end() || it->second.id != id) return;
        auto waiters = std::move(it->second.waiters);
        pending.erase(it);
        const bool allow = result == NetworkGateResult::allow && !cancelled;
        base::logger("exec")->debug("network proxy: {} {}", key,
                                    allow ? "allowed" : result == NetworkGateResult::cancel ? "cancelled" : "denied");
        if (allow) allowed.insert(key);
        if (result == NetworkGateResult::cancel && !cancelled) {
            cancelled = true;
            gates.request_stop();
            if (on_cancel) on_cancel();
        }
        for (auto& waiter : waiters) waiter(allow, reason);
    }

    void accept() {
        acceptor.async_accept([this](error_code ec, tcp::socket socket) {
            if (ec == asio::error::operation_aborted || !acceptor.is_open()) return;
            if (!ec) serve(std::move(socket));
            else base::logger("exec")->debug("network proxy accept: {}", ec.message());
            accept();
        });
    }

    void serve(tcp::socket socket) {
        auto session = std::make_shared<Session>(*this, std::move(socket));
        std::erase_if(sessions, [](const std::weak_ptr<Session>& weak) { return weak.expired(); });
        sessions.push_back(session);
        error_code ec;
        const auto peer = session->client.remote_endpoint(ec);
        if (ec || !detail::loopback_peer_is_current_user(listen_port, peer.port())) {
            base::logger("exec")->warn("network proxy: rejected a connection from another user");
            return session->reply_and_close(response("403 Forbidden", "connection from another user\n"));
        }
        session->read_head();
    }

    void shutdown() {
        error_code ignored;
        acceptor.close(ignored);
        for (const auto& weak : sessions)
            if (const auto session = weak.lock()) session->close();
        pending.clear();
        work.reset();
        context.stop();
    }

    // io_context 先构造、最后析构：会话与计时器的 socket/服务依赖它。
    asio::io_context context;
    asio::executor_work_guard<asio::io_context::executor_type> work = asio::make_work_guard(context);
    tcp::acceptor acceptor{context};
    unsigned short listen_port = 0;

    NetworkGate gate;
    Limits limits;
    std::function<void()> on_cancel;
    std::stop_source gates;

    // 以下状态只在服务线程上访问。
    std::set<std::string> allowed;
    std::map<std::string, Pending> pending;
    std::vector<std::weak_ptr<Session>> sessions;
    int asked = 0;
    std::uint64_t next_id = 0;
    bool expired = false;
    bool cancelled = false;

    std::mutex deciders_mutex;
    std::list<std::jthread> deciders;
    std::thread service;
    std::atomic<bool> stopped{false};
};

NetworkProxy::NetworkProxy(NetworkGate gate, Limits limits, std::function<void()> on_cancel)
    : impl_(std::make_unique<Impl>(std::move(gate), limits, std::move(on_cancel))) {
    try {
        impl_->acceptor.open(tcp::v4());
        impl_->acceptor.bind({asio::ip::address_v4::loopback(), 0});
        impl_->acceptor.listen();
        impl_->listen_port = impl_->acceptor.local_endpoint().port();
    } catch (const boost::system::system_error& e) {
        throw ExecError{ExecError::Kind::sandbox, std::string("network proxy: ") + e.what()};
    }
    impl_->accept();
    impl_->service = std::thread([impl = impl_.get()] { impl->context.run(); });
}

NetworkProxy::~NetworkProxy() { stop(); }

std::string NetworkProxy::url() const { return std::format("http://127.0.0.1:{}", impl_->listen_port); }

unsigned short NetworkProxy::port() const noexcept { return impl_->listen_port; }

void NetworkProxy::stop() noexcept {
    if (impl_->stopped.exchange(true)) return;
    impl_->gates.request_stop();
    asio::post(impl_->context, [impl = impl_.get()] { impl->shutdown(); });
    if (impl_->service.joinable()) impl_->service.join();
    // 闸门已收到 stop_token；它们的结果投递到已停止的 io_context，不再处理。
    const std::lock_guard lock(impl_->deciders_mutex);
    impl_->deciders.clear();
}

} // namespace dagent::exec
