#include "net/http.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <mutex>

#include <curl/curl.h>

namespace dagent::net {

namespace {

using Kind = HttpError::Kind;
using DataFn = std::function<void(std::string_view)>;

// 一次传输的上下文，经 CURLOPT_HEADERDATA / WRITEDATA 交给回调。
struct Transfer {
    CURL* easy = nullptr;
    const DataFn* on_data = nullptr; ///< 为空表示整包收取（send）
    std::size_t limit = 0;           ///< send：响应体上限；stream：错误体截断长度
    HttpResponse resp;
    bool too_large = false;
    std::exception_ptr error; ///< on_data 抛出的异常，不能穿过 libcurl 的 C 栈帧
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

std::size_t on_header(char* p, std::size_t size, std::size_t n, void* ud) {
    auto& t = *static_cast<Transfer*>(ud);
    const std::string_view ln(p, size * n);
    if (ln.starts_with("HTTP/")) { // 新一轮状态行（如 1xx 之后的最终响应），丢弃前一轮的头
        t.resp.headers.clear();
        return size * n;
    }
    const std::size_t colon = ln.find(':');
    if (colon == std::string_view::npos) return size * n;
    std::string name(trim(ln.substr(0, colon)));
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
    t.resp.headers.emplace_back(std::move(name), std::string(trim(ln.substr(colon + 1))));
    return size * n;
}

std::size_t on_write(char* p, std::size_t size, std::size_t n, void* ud) {
    auto& t = *static_cast<Transfer*>(ud);
    const std::size_t len = size * n;
    if (t.on_data) {
        long status = 0;
        curl_easy_getinfo(t.easy, CURLINFO_RESPONSE_CODE, &status);
        if (status >= 200 && status < 300) {
            try {
                (*t.on_data)(std::string_view(p, len));
            } catch (...) {
                t.error = std::current_exception();
                return CURL_WRITEFUNC_ERROR;
            }
        } else {
            t.resp.body.append(p, std::min(len, t.limit - std::min(t.limit, t.resp.body.size())));
        }
        return len;
    }
    if (t.resp.body.size() + len > t.limit) {
        t.too_large = true;
        return CURL_WRITEFUNC_ERROR;
    }
    t.resp.body.append(p, len);
    return len;
}

Kind classify(CURLcode rc, bool too_large) {
    switch (rc) {
    case CURLE_OPERATION_TIMEDOUT:
        return Kind::timeout;
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
        return Kind::connect;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CACERT_BADFILE:
        return Kind::tls;
    case CURLE_WRITE_ERROR:
        return too_large ? Kind::too_large : Kind::transport;
    default:
        return Kind::transport;
    }
}

} // namespace

std::optional<std::string_view> HttpResponse::header(std::string_view name) const {
    for (const auto& [k, v] : headers)
        if (k == name) return v;
    return std::nullopt;
}

struct HttpClient::Impl {
    CURL* easy = curl_easy_init();
    CURLM* multi = curl_multi_init(); ///< 持有连接缓存；用 multi 接口是为了 poll 能被 wakeup 即时打断
    char errbuf[CURL_ERROR_SIZE] = {};

    ~Impl() {
        curl_multi_cleanup(multi);
        curl_easy_cleanup(easy);
    }

    HttpResponse run(const HttpOptions& opt, const HttpRequest& req, const DataFn* on_data,
                     std::stop_token stop);
};

HttpResponse HttpClient::Impl::run(const HttpOptions& opt, const HttpRequest& req,
                                   const DataFn* on_data, std::stop_token stop) {
    Transfer t;
    t.easy = easy;
    t.on_data = on_data;
    t.limit = on_data ? opt.max_error_body_bytes : opt.max_body_bytes;

    curl_slist* hdrs = nullptr;
    for (const auto& [k, v] : req.headers) hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());
    hdrs = curl_slist_append(hdrs, "Expect:"); // 大请求体不等 100-continue，直接发
    const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> hdrs_guard(hdrs, curl_slist_free_all);

    curl_easy_reset(easy); // 只清选项，连接缓存在 multi 上保留
    errbuf[0] = '\0';
    curl_easy_setopt(easy, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
    if (req.method == "GET") {
        curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
    } else {
        if (req.method == "POST" || !req.body.empty()) {
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, req.body.data());
        }
        if (req.method != "POST") curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    }
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "DAgent/0.1");
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, static_cast<long>(opt.timeout.count()));
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, static_cast<long>(opt.connect_timeout.count()));
    if (opt.idle_timeout.count() > 0) {
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, static_cast<long>(opt.idle_timeout.count()));
    }
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, opt.verify_peer ? 1L : 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, opt.verify_host ? 2L : 0L);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &t);

    curl_multi_add_handle(multi, easy);
    bool cancelled = false;
    {
        const std::stop_callback wake(stop, [m = multi] { curl_multi_wakeup(m); });
        int running = 1;
        while (running) {
            if (stop.stop_requested()) {
                cancelled = true;
                break;
            }
            curl_multi_perform(multi, &running);
            if (running) curl_multi_poll(multi, nullptr, 0, 1000, nullptr);
        }
    }
    CURLcode rc = CURLE_OK;
    int left = 0;
    while (const CURLMsg* msg = curl_multi_info_read(multi, &left))
        if (msg->msg == CURLMSG_DONE) rc = msg->data.result;
    curl_multi_remove_handle(multi, easy);

    if (t.error) std::rethrow_exception(t.error);
    if (cancelled) throw HttpError(Kind::cancelled, req.method + " " + req.url + " interrupted");
    if (rc != CURLE_OK) {
        const std::string detail = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        throw HttpError(classify(rc, t.too_large), req.method + " " + req.url + " failed: " + detail);
    }
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &t.resp.status);
    return std::move(t.resp);
}

HttpClient::HttpClient(HttpOptions opt) : opt_(opt) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    impl_ = std::make_unique<Impl>();
}

HttpClient::~HttpClient() = default;

HttpResponse HttpClient::send(const HttpRequest& req, std::stop_token stop) {
    return impl_->run(opt_, req, nullptr, std::move(stop));
}

HttpResponse HttpClient::stream(const HttpRequest& req, const std::function<void(std::string_view)>& on_data,
                                std::stop_token stop) {
    return impl_->run(opt_, req, &on_data, std::move(stop));
}

} // namespace dagent::net
