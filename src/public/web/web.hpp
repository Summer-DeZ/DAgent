#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "exec/mxc.hpp"

namespace dagent::web {

struct Options {
    std::filesystem::path curl;
    std::chrono::milliseconds timeout{120000};
    std::chrono::milliseconds startup_timeout{30000};
    std::size_t max_body_bytes = 2 << 20;
    std::size_t cache_bytes = 16 << 20;
    std::vector<std::string> engines{"yahoo", "brave", "duckduckgo"};
};

struct Response {
    std::string url, content_type, body;
    int status = 0;
    bool truncated = false;
};

struct Page {
    std::string url, title, content_type, text;
    int status = 0;
    bool truncated = false;
};

struct SearchResult { std::string title, url, snippet; };
struct SearchResults {
    std::vector<SearchResult> items;
    std::vector<std::string> failures;
};

/// @brief 逐跳请求前的回调：审阅当前完整 URL，并替换该跳请求的网络闸门。
/// 拒绝/取消按现有 web/exec 错误转换返回，不能继续发送。
using BeforeRequest = std::function<void(
    std::string_view normalized_url,
    std::size_t redirect_index,
    exec::MxcRequest& request,
    std::stop_token)>;

// Every request is performed by curl inside the caller's granted sandbox boundary.
// The request is sent one hop at a time; `before` runs before each hop.
Response get(exec::MxcRequest request, std::string_view url, const Options&,
             exec::Options process, std::stop_token = {});
Response get(exec::MxcRequest request, std::string_view url, const Options&,
             exec::Options process, const BeforeRequest& before, std::stop_token = {});
Page decode(Response);
SearchResults search_results(std::string_view json, std::size_t limit);
std::string search_url(std::string_view endpoint, std::string_view query);
// WHATWG canonicalization; rejects non-HTTP(S) and credentials.
std::string normalize_url(std::string_view url, std::string_view base = {});
// 规范化 URL 的 host 与端口（http→80、https→443，显式端口优先，IPv6 返回 [] 内文本）；失败返回 nullopt。
std::optional<std::pair<std::string, int>> url_target(std::string_view url);

} // namespace dagent::web
