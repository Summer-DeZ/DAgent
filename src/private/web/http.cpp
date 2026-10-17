#include "web/web.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <mutex>
#include <optional>
#include <utility>
#include "base/text.hpp"
#include "lib/nlohmann/json.hpp"

namespace dagent::web {
namespace {

/// @brief HTTP 头名比较；大小写不敏感。
bool header_name_is(std::string_view name, std::string_view expected) {
    if (name.size() != expected.size()) return false;
    for (std::size_t i = 0; i < name.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(name[i])) !=
            std::tolower(static_cast<unsigned char>(expected[i]))) return false;
    return true;
}

/// @brief 取一个响应头块里的头值；没有该头时返回 nullopt。
std::optional<std::string> header_value(std::string_view block, std::string_view name) {
    std::size_t pos = 0;
    while (pos < block.size()) {
        auto end = block.find('\n', pos);
        if (end == std::string_view::npos) end = block.size();
        auto line = block.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) break;
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        if (!header_name_is(line.substr(0, colon), name)) continue;
        auto value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        return std::string(value);
    }
    return std::nullopt;
}

/// @brief curl --include 输出拆出的最后一个响应头块与正文。
struct Included {
    std::string_view head, body;
};

/// @brief 解析最后一个响应头块；1xx 中间响应会连续出现多个头块，正文以最后一个块为准。
Included split_include(std::string_view out) {
    std::size_t cursor = 0, begin = 0, end = 0;
    bool found = false;
    while (out.size() - cursor >= 5 && out.compare(cursor, 5, "HTTP/") == 0) {
        auto separator = out.find("\r\n\r\n", cursor);
        std::size_t delimiter = 4;
        if (separator == std::string_view::npos) {
            separator = out.find("\n\n", cursor);
            delimiter = 2;
        }
        if (separator == std::string_view::npos) break;
        begin = cursor;
        end = separator;
        cursor = separator + delimiter;
        found = true;
        const auto status = out.find(' ', begin);
        if (status == std::string_view::npos || status >= end || out[status + 1] != '1') break;
    }
    return found ? Included{out.substr(begin, end - begin), out.substr(cursor)} : Included{{}, out};
}

} // namespace

std::optional<std::pair<std::string, int>> url_target(std::string_view url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return std::nullopt;
    const auto scheme = url.substr(0, scheme_end);
    int port = scheme == "https" ? 443 : scheme == "http" ? 80 : -1;
    if (port < 0) return std::nullopt;
    const auto rest = url.substr(scheme_end + 3);
    const auto authority = rest.substr(0, rest.find_first_of("/?#"));
    if (authority.empty()) return std::nullopt;
    std::string host;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        host = std::string(authority.substr(1, close - 1));
        const auto tail = authority.substr(close + 1);
        if (!tail.empty()) {
            if (tail.front() != ':') return std::nullopt;
            try { port = std::stoi(std::string(tail.substr(1))); } catch (const std::exception&) { return std::nullopt; }
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            host = std::string(authority);
        } else {
            host = std::string(authority.substr(0, colon));
            try { port = std::stoi(std::string(authority.substr(colon + 1))); } catch (const std::exception&) { return std::nullopt; }
        }
    }
    if (host.empty()) return std::nullopt;
    return std::make_pair(std::move(host), port);
}

Response get(exec::SrtRequest request, std::string_view url, const Options& options,
             exec::Options process, std::stop_token stop) {
    return get(std::move(request), url, options, std::move(process), BeforeRequest{}, stop);
}

Response get(exec::SrtRequest request, std::string_view url, const Options& options,
             exec::Options process, const BeforeRequest& before, std::stop_token stop) {
    if (request.policy.mode != exec::Mode::read_only)
        throw std::runtime_error("web transport requires read-only SRT");
    std::string current = normalize_url(url);
    request.environment.emplace_back("LC_ALL", "C.UTF-8");
    process.max_output_bytes = std::max(options.max_body_bytes + 1, std::size_t{65536});
    // 整体预算按绝对截止时间收窄；每一跳只用剩余时间。
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    std::string denial;
    std::mutex mutex;
    for (std::size_t redirect_index = 0;; ++redirect_index) {
        if (stop.stop_requested())
            throw exec::ExecError(exec::ExecError::Kind::cancelled, "web request cancelled");
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero())
            throw std::runtime_error("web request timed out (including time waiting for approval)");
        // 逐跳审阅：交给回调替换本跳闸门；回调拒绝/抛出时按现有异常路径返回，不发送。
        if (before) before(current, redirect_index, request, stop);
        remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero())
            throw std::runtime_error("web request timed out while waiting for approval");
        denial.clear();
        request.network_gate = [gate = request.network_gate, &denial, &mutex](std::string_view host, int port,
            std::string& reason, std::stop_token token) {
            const auto action = gate ? gate(host, port, reason, token) : exec::NetworkGateResult::deny;
            if (action != exec::NetworkGateResult::allow) {
                const std::lock_guard lock(mutex);
                denial = std::format("{}:{}: {}", host, port,
                    reason.empty() ? "network approval is unavailable" : reason);
            }
            return action;
        };
        // stderr carries metadata independently of the bounded body pipe. -q disables curlrc.
        // --include 保留响应头供本层读 Location；不跟随跳转，每次只跑一跳。
        request.command = exec::shell_quote(options.curl.string()) +
            " -q --silent --show-error --globoff --max-redirs 0 --include --suppress-connect-headers --output -"
            " --proto '=http,https' --proto-redir '=http,https' --noproxy '' --compressed"
            " --connect-timeout 15 --user-agent 'DAgent/0.1' --write-out '%{stderr}\\nDAGENT_WEB_META:%{json}\\n' --url " +
            exec::shell_quote(current) + " | /usr/bin/head -c " + std::to_string(options.max_body_bytes + 1);
        request.timeout = remaining;
        exec::Result result;
        try {
            result = exec::run_srt(request, process, {}, stop);
        } catch (const exec::ExecError&) {
            if (!denial.empty() && !stop.stop_requested()) throw std::runtime_error(denial);
            throw;
        }
        if (!denial.empty()) throw std::runtime_error(denial);
        if (result.timed_out) throw std::runtime_error("web request timed out (including time waiting for approval)");
        constexpr std::string_view marker = "DAGENT_WEB_META:";
        const auto pos = result.err.rfind(marker);
        if (pos == std::string::npos)
            throw std::runtime_error("curl returned no response metadata: " + base::to_valid_utf8(result.err));
        const auto begin = pos + marker.size();
        const auto metadata = nlohmann::json::parse(result.err.substr(begin, result.err.find('\n', begin) - begin));
        const auto included = split_include(result.out);
        Response response;
        response.url = metadata.at("url_effective").get<std::string>();
        if (const auto& type = metadata.at("content_type"); type.is_string()) response.content_type = type.get<std::string>();
        response.status = metadata.at("http_code").get<int>();
        response.truncated = result.out.size() > options.max_body_bytes;
        const int code = metadata.at("exitcode").get<int>();
        if (code != 0 && !(code == 23 && response.truncated))
            throw std::runtime_error(std::format("curl failed ({}): {} ({} body bytes); {}", code,
                metadata.value("errormsg", "transport failed"), included.body.size(),
                base::to_valid_utf8(result.err.substr(0, pos))));
        if (response.status >= 300 && response.status < 400) {
            const auto location = header_value(included.head, "location");
            if (location && !location->empty()) {
                if (redirect_index >= 10) throw std::runtime_error("too many redirects (limit 10)");
                // 相对/绝对 Location 都按当前 URL 规范化；同一 host 的不同 URL 下一跳重新走审阅。
                current = normalize_url(*location, current);
                continue;
            }
        }
        if (response.status < 200 || response.status >= 300)
            throw std::runtime_error(std::format("HTTP {} from {}", response.status, response.url));
        response.body = std::string(included.body.substr(0, options.max_body_bytes));
        return response;
    }
}

std::string search_url(std::string_view endpoint, std::string_view query) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result(endpoint);
    result += "/search?format=json&q=";
    for (const unsigned char c : query) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') result += c;
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}

SearchResults search_results(std::string_view body, std::size_t limit) {
    const auto data = nlohmann::json::parse(body);
    SearchResults out;
    for (const auto& item : data.at("results")) {
        if (out.items.size() == limit) break;
        auto url = item.value("url", "");
        try { url = normalize_url(url); } catch (const std::exception&) { continue; }
        out.items.push_back({item.value("title", ""), std::move(url), item.value("content", "")});
    }
    for (const auto& item : data.value("unresponsive_engines", nlohmann::json::array()))
        if (item.is_array() && item.size() >= 2)
            out.failures.push_back(item.at(0).get<std::string>() + ": " + item.at(1).get<std::string>());
    return out;
}

} // namespace dagent::web
