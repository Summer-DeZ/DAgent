#include "web/web.hpp"

#include <algorithm>
#include <format>
#include <mutex>
#include "base/text.hpp"
#include "lib/nlohmann/json.hpp"

namespace dagent::web {

Response get(exec::SrtRequest request, std::string_view url, const Options& options,
             exec::Options process, std::stop_token stop) {
    const auto normalized = normalize_url(url);
    if (request.policy.mode != exec::Mode::read_only)
        throw std::runtime_error("web transport requires read-only SRT");
    std::string denial;
    std::mutex mutex;
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
    request.command = exec::shell_quote(options.curl.string()) +
        " -q --silent --show-error --globoff --location --max-redirs 10 --output -"
        " --proto '=http,https' --proto-redir '=http,https' --noproxy '' --compressed"
        " --connect-timeout 15 --user-agent 'DAgent/0.1' --write-out '%{stderr}\\nDAGENT_WEB_META:%{json}\\n' --url " +
        exec::shell_quote(normalized) + " | /usr/bin/head -c " + std::to_string(options.max_body_bytes + 1);
    request.timeout = options.timeout;
    request.environment.emplace_back("LC_ALL", "C.UTF-8");
    process.max_output_bytes = std::max(options.max_body_bytes + 1, std::size_t{65536});
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
    Response response;
    response.url = metadata.at("url_effective").get<std::string>();
    if (const auto& type = metadata.at("content_type"); type.is_string()) response.content_type = type.get<std::string>();
    response.status = metadata.at("http_code").get<int>();
    response.truncated = result.out.size() > options.max_body_bytes;
    const int code = metadata.at("exitcode").get<int>();
    if (code != 0 && !(code == 23 && response.truncated))
        throw std::runtime_error(std::format("curl failed ({}): {} ({} body bytes); {}", code,
            metadata.value("errormsg", "transport failed"), result.out.size(),
            base::to_valid_utf8(result.err.substr(0, pos))));
    if (response.status < 200 || response.status >= 300)
        throw std::runtime_error(std::format("HTTP {} from {}", response.status, response.url));
    response.body = result.out.substr(0, options.max_body_bytes);
    return response;
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
