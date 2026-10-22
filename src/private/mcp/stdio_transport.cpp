#include "mcp/transport.hpp"

#include <format>
#include <utility>

#include "base/log.hpp"
#include "exec/mxc.hpp"
#include "mcp/detail.hpp"

namespace dagent::mcp::detail {

using json = nlohmann::json;
using std::chrono::milliseconds;

StdioTransport::StdioTransport(const ServerConfig& config, const Options& opt, ConnectionState& connection)
    : config_(config), opt_(opt), connection_(connection) {}

StdioTransport::~StdioTransport() = default;

void StdioTransport::start() {
    if (config_.command.empty())
        throw McpError{McpError::Kind::spawn, config_.name + ": stdio server is missing command"};
    if (!config_.sandbox)
        throw McpError{McpError::Kind::spawn,
                       config_.name + ": " +
                           (config_.sandbox_unavailable.empty()
                                ? "no permission profile is configured; the server was not started"
                                : config_.sandbox_unavailable)};
    start_sandboxed();
}

void StdioTransport::start_sandboxed() {
    const StdioSandbox& sandbox = *config_.sandbox;
    exec::MxcRequest request;
    request.runtime = sandbox.runtime;
    request.workspace = config_.cwd;
    request.policy = sandbox.policy;
    for (const std::string& argument : config_.command) {
        if (!request.command.empty()) request.command += ' ';
        request.command += exec::shell_quote(argument);
    }
    // HOME/XDG/TMPDIR 由 MxcChild 指向每连接私有目录；project 没有托管 PATH，与受限 bash 一样使用项目搜索路径。
    // server 的 env 最后覆盖。
    if (config_.environment == "project") request.environment.emplace_back("PATH", exec::project_search_path());
    request.environment.insert(request.environment.end(), config_.env.begin(), config_.env.end());
    // profile 的网络目标只经本连接的回环代理严格放行，没有运行中审批；没有目标时完全无网络。
    if (!sandbox.policy.network_targets.empty())
        request.network_gate = exec::allowlist_gate(sandbox.policy.network_targets);
    try {
        auto process = opt_.process;
        if (config_.environment == "project") process.environment.clear();
        child_ = exec::MxcChild::spawn(request, process);
    } catch (const exec::ExecError& e) {
        throw McpError{McpError::Kind::spawn, config_.name + ": " + e.what()};
    }
    child_->on_line([this](std::string_view line) { on_line(line); });
    child_->on_exit([this](std::optional<int> code, std::optional<int> signal) { on_exit(code, signal); });
    child_->on_stderr([](std::string_view line) { base::logger("mcp")->info("{}", line); }); // server 自身的 stderr
}

void StdioTransport::shutdown(const CloseProtocol& close_protocol) {
    std::unique_ptr<exec::MxcChild> child;
    {
        std::lock_guard lock(connection_.mutex);
        if (connection_.closing) return;
        connection_.closing = true;
        child = std::move(child_);
        connection_.disconnected = true;
        for (auto& [id, pending] : pending_) {
            if (pending->done) continue;
            pending->done = true;
            pending->disconnected = true;
            pending->error_text = config_.name + ": client is closed";
        }
    }
    cv_.notify_all();
    close_protocol();
    if (child) child->terminate(); // 锁外等读取线程退出并清理私有目录，之后不会再回调
    child.reset();
}

void StdioTransport::send(std::string_view line) {
    std::string data(line);
    data.push_back('\n');
    const std::lock_guard lock(connection_.mutex);
    if (!child_) return;
    child_->write(data);
}

void StdioTransport::on_line(std::string_view line) {
    json message = json::parse(line, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        base::logger("mcp")->warn("{}: 忽略 server stdout 上的非 JSON 行（{} 字节）", config_.name, line.size());
        return;
    }
    if (!message.contains("method")) {
        const auto id = message.find("id");
        if (id == message.end() || !id->is_number_integer()) {
            base::logger("mcp")->debug("{}: 忽略无法匹配的消息", config_.name);
            return;
        }
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard lock(connection_.mutex);
            const auto it = pending_.find(id->get<std::int64_t>());
            if (it == pending_.end()) {
                base::logger("mcp")->debug("{}: 忽略已结束请求 {} 的迟到响应", config_.name, id->get<std::int64_t>());
                return;
            }
            pending = it->second;
            if (message.contains("error")) pending->error = message["error"];
            else pending->result = message.contains("result") ? std::move(message["result"]) : json::object();
            pending->done = true;
        }
        cv_.notify_all();
        return;
    }
    const json& method = message["method"];
    base::logger("mcp")->debug("{}: 忽略主动消息 {}", config_.name,
                              method.is_string() ? method.get<std::string>() : std::string{});
}

void StdioTransport::on_exit(std::optional<int> code, std::optional<int> signal) {
    std::string reason;
    if (signal.has_value()) reason = std::format("server terminated by signal {}", *signal);
    else reason = std::format("server exited (code={})", code.value_or(-1));
    {
        std::lock_guard lock(connection_.mutex);
        connection_.disconnected = true;
        connection_.disconnect_reason = config_.name + ": " + reason;
        for (auto& [id, pending] : pending_) {
            if (pending->done) continue;
            pending->done = true;
            pending->disconnected = true;
            pending->error_text = connection_.disconnect_reason;
        }
    }
    cv_.notify_all();
    base::logger("mcp")->warn("{}", connection_.disconnect_reason);
}

Response StdioTransport::exchange(std::int64_t id, std::string_view method, json params,
                                  milliseconds timeout, std::stop_token stop) {
    std::string line = detail::make_request(id, method, std::move(params)).dump();
    auto pending = std::make_shared<Pending>();
    {
        std::lock_guard lock(connection_.mutex);
        if (connection_.closing)
            throw McpError{McpError::Kind::disconnected, config_.name + ": client is closed"};
        if (connection_.disconnected) throw McpError{McpError::Kind::disconnected, connection_.disconnect_reason};
        pending_[id] = pending;
    }
    send(line);

    {
        std::unique_lock lock(connection_.mutex);
        const auto done = [&pending] { return pending->done; };
        if (timeout.count() > 0) (void)cv_.wait_for(lock, stop, timeout, done);
        else (void)cv_.wait(lock, stop, done);
        if (!pending->done) {
            pending_.erase(id);
            lock.unlock();
            if (stop.stop_requested()) {
                send_cancel(id, "cancelled by caller");
                throw McpError{McpError::Kind::cancelled,
                               std::format("{}: {} interrupted", config_.name, method)};
            }
            send_cancel(id, "timeout");
            throw McpError{McpError::Kind::timeout,
                           std::format("{}: {} timed out ({}ms)", config_.name, method, timeout.count())};
        }
        pending_.erase(id);
    }
    if (pending->disconnected) throw McpError{McpError::Kind::disconnected, pending->error_text};
    Response out;
    if (!pending->error.is_null()) {
        out.rpc_error = true;
        out.error = std::move(pending->error);
    } else {
        out.ok = true;
        out.result = std::move(pending->result);
    }
    return out;
}

void StdioTransport::send_cancel(std::int64_t id, std::string_view reason) {
    try {
        const json message = detail::make_notification(
            "notifications/cancelled", {{"requestId", id}, {"reason", std::string(reason)}});
        send(message.dump());
    } catch (const std::exception& e) {
        base::logger("mcp")->warn("{}: 发送 notifications/cancelled 失败：{}", config_.name, e.what());
    }
}

Response StdioTransport::notify(std::string_view method, json params, std::stop_token stop) {
    if (stop.stop_requested())
        throw McpError{McpError::Kind::cancelled, config_.name + ": connection interrupted"};
    send(detail::make_notification(method, std::move(params)).dump());
    Response response;
    response.ok = true;
    return response;
}

} // namespace dagent::mcp::detail
