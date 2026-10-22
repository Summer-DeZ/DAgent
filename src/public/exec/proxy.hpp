/// @file proxy.hpp
/// @brief 每次受限执行独占的回环 HTTP 代理：MXC 只保证容器除了这个端点没有任何出口，
/// 目的地允许与否由 NetworkGate 按 host:port 判定。
///
/// 协议：`CONNECT host:port` 建立隧道（TLS 端到端，不解密）；绝对形式的明文 HTTP 请求
/// （`GET http://host[:port]/path`）按 origin 形式转发到目标；其它请求回 400。
/// 判定规则：同一 host:port 在本代理生命周期内只向闸门提问一次（允许结果缓存，拒绝不缓存）；
/// 提问次数超过 max_requests 后一律拒绝；等待闸门超过 approval_timeout 视为拒绝，并且之后不再提问；
/// 闸门返回 cancel 时回 403 并调用 on_cancel，调用方据此终止受限命令。拒绝以 403 回应 CONNECT 或明文请求。
/// 主机名在判定之后才在宿主解析（闸门拿到的是请求里写的名字）。
/// 对端必须属于当前用户（exec::detail::loopback_peer_is_current_user），否则 403 并关闭。
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "exec/sandbox.hpp"

namespace dagent::exec {

class NetworkProxy {
public:
    struct Limits {
        std::chrono::milliseconds approval_timeout{120000};
        int max_requests = 32;
    };

    /// 监听 127.0.0.1 的内核分配端口并启动服务线程；失败抛 ExecError{sandbox}。
    /// gate 为空时拒绝全部目标。on_cancel 在服务线程上最多调用一次。
    NetworkProxy(NetworkGate gate, Limits limits, std::function<void()> on_cancel);
    NetworkProxy(const NetworkProxy&) = delete;
    NetworkProxy& operator=(const NetworkProxy&) = delete;
    ~NetworkProxy(); ///< stop()

    /// `http://127.0.0.1:<port>`，供 runtimeConfig.networkProxy 使用。
    std::string url() const;
    unsigned short port() const noexcept;

    /// 关闭监听与全部隧道，等待服务线程结束；可重复调用。正在等待闸门的请求按拒绝结束。
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dagent::exec
