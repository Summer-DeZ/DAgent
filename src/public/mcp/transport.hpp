/// @file transport.hpp
/// @brief MCP 内部传输状态与 stdio 实现；不属于 Client 对外接口。
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

namespace dagent::exec { class Child; }

namespace dagent::mcp::detail {

/// 一次请求的原始结果；超时/取消/断连以异常表达，业务错误放在这里。
struct Response {
    bool ok = false;
    nlohmann::json result;
    bool rpc_error = false;
    nlohmann::json error;
    long status = 0; ///< 仅 HTTP
};

/// Impl 持有、传输借用；stdio 的 pending 与 Child 读写也由这一把锁保护。
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
    void on_bridge_line(std::string_view line);
    void send(std::string_view line);
    void send_cancel(std::int64_t id, std::string_view reason);
    void on_line(std::string_view line);
    void on_exit(std::optional<int> code, std::optional<int> signal);

    const ServerConfig& config_;
    const Options& opt_;
    ConnectionState& connection_;
    std::unique_ptr<exec::Child> child_;
    std::filesystem::path state_dir_; ///< 受限 stdio 的每次连接私有目录
    std::filesystem::path private_home_; ///< SRT Unix sockets 使用的短路径私有 HOME
    std::mutex bridge_mu_;
    std::condition_variable_any bridge_cv_;
    bool bridge_ready_ = false;
    bool bridge_failed_ = false;
    std::string bridge_error_;
    std::condition_variable_any cv_;
    std::map<std::int64_t, std::shared_ptr<Pending>> pending_;
};

} // namespace dagent::mcp::detail
