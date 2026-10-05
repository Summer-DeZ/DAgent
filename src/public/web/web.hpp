#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>
#include "exec/srt.hpp"

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

// Every request is performed by curl inside the caller's granted SRT boundary.
Response get(exec::SrtRequest request, std::string_view url, const Options&,
             exec::Options process, std::stop_token = {});
Page decode(Response);
SearchResults search_results(std::string_view json, std::size_t limit);
std::string search_url(std::string_view endpoint, std::string_view query);
// WHATWG canonicalization; rejects non-HTTP(S) and credentials.
std::string normalize_url(std::string_view url, std::string_view base = {});

} // namespace dagent::web
