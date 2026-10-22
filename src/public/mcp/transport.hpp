/// @file transport.hpp
/// @brief MCP 内部传输状态与 stdio/HTTP 实现；不属于 Client 对外接口。
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "mcp/client.hpp"

namespace dagent::exec { class MxcChild; }

namespace dagent::mcp::detail {

/// 一次请求的原始结果；超时/取消/断连以异常表达，业务错误放在这里。
struct Response {
    bool ok = false;
    nlohmann::json result;
    bool rpc_error = false;
    nlohmann::json error;
    long status = 0; ///< 仅 HTTP
};

/// Impl 持有、传输借用；stdio 的 pending 与 MxcChild 读写也由这一把锁保护。
struct ConnectionState {
    std::mutex mutex;
    bool closing = false;
    bool disconnected = false;
    std::string disconnect_reason;
};

using CloseProtocol = std::function<void()>;

/// 不持有 Impl 或 Dialect；请求参数由协议层预先装饰。
/// 所有者须在销毁本对象及借用状态前调用 shutdown，等待进程回调结束。
class StdioTransport {
public:
    StdioTransport(const ServerConfig&, const Options&, ConnectionState&);
    ~StdioTransport();

    void start();
    Response exchange(std::int64_t id, std::string_view method, nlohmann::json params,
                      std::chrono::milliseconds timeout, std::stop_token stop);
    Response notify(std::string_view method, nlohmann::json params, std::stop_token stop);
    void shutdown(const CloseProtocol&);

private:
    /// 读取线程填结果，调用线程取走并从 id 表删除。
    struct Pending {
        bool done = false;
        bool disconnected = false;
        std::string error_text;
        nlohmann::json result; ///< 成功时的 result；error 非 null 时无效
        nlohmann::json error;
    };

    void start_sandboxed();
    void send(std::string_view line);
    void send_cancel(std::int64_t id, std::string_view reason);
    void on_line(std::string_view line);
    void on_exit(std::optional<int> code, std::optional<int> signal);

    const ServerConfig& config_;
    const Options& opt_;
    ConnectionState& connection_;
    std::unique_ptr<exec::MxcChild> child_;
    std::condition_variable_any cv_;
    std::map<std::int64_t, std::shared_ptr<Pending>> pending_;
};

using HeaderFactory = std::function<net::Headers()>;
using HeaderObserver = std::function<void(const net::HttpResponse&)>;

/// 每次请求独占 HttpClient 与取消/计时状态；不持有 Impl 或 Dialect。
class HttpTransport {
public:
    HttpTransport(const ServerConfig&, const Options&, ConnectionState&);

    void start();
    /// 两个协议回调仅在本次同步 exchange 内调用，不保存为成员。
    Response exchange(std::optional<std::int64_t> id, std::string_view method,
                      const nlohmann::json& message, std::chrono::milliseconds timeout,
                      std::stop_token stop, const HeaderFactory&, const HeaderObserver&);
    /// 在连接标记 closing 后仍可发送 DELETE，不经普通请求入口。
    void close_session(const net::Headers&);
    void shutdown(const CloseProtocol&);

private:
    const ServerConfig& config_;
    const Options& opt_;
    ConnectionState& connection_;
};

} // namespace dagent::mcp::detail
