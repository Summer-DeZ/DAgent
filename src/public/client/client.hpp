/// @file client.hpp
/// @brief 前端 RPC 客户端：请求匹配、事件/交互转交与连接结束报告。
///
/// 读取线程只做匹配与回调转交；事件按协议交给前端（由前端 post 到自己的线程）。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "ipc/channel.hpp"
#include "lib/nlohmann/json.hpp"
#include "protocol/dto.hpp"
#include "protocol/rpc.hpp"

namespace dagent::client {

/// @brief 后端返回的业务/协议错误。
class RpcFailure : public std::runtime_error {
public:
    explicit RpcFailure(protocol::RpcError error)
        : std::runtime_error(error.message), error_(std::move(error)) {}

    const protocol::RpcError& error() const noexcept { return error_; }

private:
    protocol::RpcError error_;
};

class Client {
public:
    struct Callbacks {
        std::function<void(const protocol::Event&)> event;
        std::function<void(const protocol::InteractionRequest&)> interaction;
        std::function<void(const std::string&)> interaction_closed;
        /// @brief 连接结束（EOF/写失败）；实现在自己的线程上处理。
        std::function<void(const std::string&)> disconnected;
    };

    Client(ipc::Channel channel, Callbacks callbacks);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// @brief 同步请求；后端返回错误抛 RpcFailure，连接结束抛 std::runtime_error。
    nlohmann::json call(const std::string& method, nlohmann::json params = nlohmann::json::object(),
                        std::chrono::milliseconds timeout = {});

    /// @brief 异步请求；响应到达时在读取线程回调（可直接 post 到 UI）。
    void call_async(std::string method, nlohmann::json params,
                    std::function<void(std::expected<nlohmann::json, protocol::RpcError>)> done);

    /// @brief 主动结束：关闭连接并 join 读取线程。
    void close();
    bool connected() const;

private:
    struct Pending;
    struct SyncPending;

    void reader(std::stop_token stop);
    void finish_all(const std::string& error, bool notify_disconnect = true);

    ipc::Channel channel_;
    Callbacks callbacks_;
    mutable std::mutex mutex_;
    std::uint64_t next_id_ = 0;
    std::map<std::string, std::shared_ptr<Pending>> pending_;
    std::map<std::string, std::shared_ptr<SyncPending>> sync_;
    bool closing_ = false;
    std::string error_;
    std::jthread reader_;
};

} // namespace dagent::client
