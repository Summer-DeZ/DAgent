/// @file http.hpp
/// @brief HTTP 客户端：libcurl 的薄封装，支持整包请求与流式接收，可经 stop_token 即时取消。
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dagent::net {

using Headers = std::vector<std::pair<std::string, std::string>>;

/// @brief 客户端选项，对应 config/dagent.json 的 "http" 段。时长为 0 表示不限。
struct HttpOptions {
    std::chrono::seconds timeout{300};         ///< 整个请求的总时长上限
    std::chrono::seconds connect_timeout{30};  ///< 建连（含 TLS 握手）上限；为 0 时取 libcurl 默认 300 秒
    std::chrono::seconds idle_timeout{0};      ///< 连续多久收不到任何字节即判超时，适合长流
    std::size_t max_body_bytes = 2 << 20;      ///< send() 响应体上限，超出抛 too_large
    std::size_t max_error_body_bytes = 4096;   ///< stream() 非 2xx 时保留的错误体上限（超出截断）
    bool verify_peer = true;
    bool verify_host = true;
};

struct HttpRequest {
    std::string method = "POST"; ///< GET / POST / 其他任意方法名
    std::string url;
    Headers headers;             ///< 原样发送；无需手写 Content-Length
    std::string body;
};

struct HttpResponse {
    long status = 0;
    Headers headers;  ///< 名字已转小写，保留重复项与出现顺序
    std::string body; ///< send()：完整响应体；stream()：仅非 2xx 时的错误体

    /// @brief 取首个同名响应头（name 须小写）。
    std::optional<std::string_view> header(std::string_view name) const;
};

/// @brief 传输层失败（未拿到完整 HTTP 响应）。HTTP 状态码错误不抛异常，由调用方看 status。
class HttpError : public std::runtime_error {
public:
    enum class Kind {
        cancelled, ///< stop_token 请求停止
        timeout,   ///< 总时长或空闲超时
        connect,   ///< 解析域名 / 建连失败
        tls,       ///< 证书或握手失败，重试无意义
        too_large, ///< 响应体超过 max_body_bytes
        transport, ///< 连接中途断开等其余错误，通常可重试
    };

    HttpError(Kind kind, const std::string& what) : std::runtime_error(what), kind_(kind) {}
    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

/// @brief 单连接复用的客户端：同一实例上的请求串行执行，连接、DNS 与 TLS 会话在请求间保留。
/// 不可跨线程并发使用；取消请从其他线程对 stop_token 所属的 stop_source 调 request_stop()。
class HttpClient {
public:
    explicit HttpClient(HttpOptions opt = {});
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    /// @brief 发送请求并读取完整响应体。
    HttpResponse send(const HttpRequest& req, std::stop_token stop = {});

    /// @brief 发送请求并流式接收：状态码为 2xx 时每到一段数据就在调用线程上回调 on_data，
    /// 否则把错误体收进返回值的 body。on_data 抛出的异常会中止传输并原样重新抛出。
    HttpResponse stream(const HttpRequest& req,
                        const std::function<void(std::string_view)>& on_data,
                        std::stop_token stop = {});

    const HttpOptions& options() const noexcept { return opt_; }

private:
    struct Impl;
    HttpOptions opt_;
    std::unique_ptr<Impl> impl_;
};

} // namespace dagent::net
