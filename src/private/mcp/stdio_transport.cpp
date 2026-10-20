#include "mcp/transport.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <utility>

#include "base/log.hpp"
#include "base/platform.hpp"
#include "exec/child.hpp"
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
                       config_.name + ": no permission profile is configured; the server was not started"};
    start_sandboxed();
}

void StdioTransport::start_sandboxed() {
    const StdioSandbox& sandbox = *config_.sandbox;
    std::error_code error;
    std::filesystem::create_directories(sandbox.state_root, error);
    auto directory = base::create_unique_directory((sandbox.state_root / ("mcp-" + config_.name + "-XXXXXX")).string());
    if (!directory)
        throw McpError{McpError::Kind::spawn, config_.name + ": cannot create the sandbox state directory"};
    state_dir_ = *directory;
    // SRT 在 TMPDIR 下创建 Unix sockets；固定短路径避免 Home 或 server 名过长。
    directory = base::create_unique_directory("/tmp/dagent-home-XXXXXX");
    if (!directory)
        throw McpError{McpError::Kind::spawn, config_.name + ": cannot create the sandbox private home"};
    private_home_ = *directory;
    const auto& private_home = private_home_;
    std::filesystem::create_directories(private_home / "config", error);
    std::filesystem::create_directories(private_home / "cache", error);
    std::filesystem::create_directories(private_home / "run", error);

    std::string command;
    for (const std::string& argument : config_.command) {
        if (!command.empty()) command += ' ';
        command += exec::shell_quote(argument);
    }
    nlohmann::json init;
    init["command"] = command;
    init["shell"] = sandbox.runtime.shell.string();
    init["config"] = exec::srt_config(sandbox.runtime, sandbox.policy, private_home, true);
    init["kill_grace_ms"] = opt_.process.kill_grace.count();
    const std::filesystem::path init_file = state_dir_ / "init.json";
    {
        std::ofstream out(init_file, std::ios::binary | std::ios::trunc);
        out << init.dump();
        if (!out) throw McpError{McpError::Kind::spawn, config_.name + ": cannot write the sandbox init file"};
    }

    exec::Command cmd;
    cmd.argv = {sandbox.runtime.node.string(), sandbox.runtime.bridge.string(), "--mode=stdio",
                "--srt=" + sandbox.runtime.entry.string(),
                "--bwrap=" + sandbox.runtime.bwrap.string(),
                "--socat=" + sandbox.runtime.socat.string(),
                "--rg=" + sandbox.runtime.rg.string(),
                "--shell=" + sandbox.runtime.shell.string(),
                "--init-file=" + init_file.string()};
    cmd.cwd = state_dir_;
    cmd.inherit_env = config_.environment == "project";
    cmd.env_set = config_.env;
    cmd.env_set.insert(cmd.env_set.end(),
                       {{"HOME", private_home.string()},
                        {"XDG_CONFIG_HOME", (private_home / "config").string()},
                        {"XDG_CACHE_HOME", (private_home / "cache").string()},
                        {"XDG_RUNTIME_DIR", (private_home / "run").string()},
                        {"TMPDIR", private_home.string()},
                        {"TMP", private_home.string()},
                        {"TEMP", private_home.string()}});
    cmd.env_unset = {"BASH_ENV", "ENV", "BASHOPTS", "SHELLOPTS", "CDPATH", "GLOBIGNORE",
                     "PROMPT_COMMAND", "LD_PRELOAD", "LD_LIBRARY_PATH", "PYTHONPATH", "PERL5LIB",
                     "RUBYOPT", "NODE_OPTIONS", "NODE_PATH", "NODE_EXTRA_CA_CERTS",
                     "http_proxy", "https_proxy", "all_proxy", "ftp_proxy",
                     "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "FTP_PROXY"};
    try {
        auto process = opt_.process;
        if (config_.environment == "project") process.environment.clear();
        child_ = exec::Child::spawn(cmd, process);
    } catch (const exec::ExecError& e) {
        throw McpError{McpError::Kind::spawn, config_.name + ": " + e.what()};
    }
    child_->on_line([this](std::string_view line) { on_line(line); });
    child_->on_exit([this](std::optional<int> code, std::optional<int> signal) { on_exit(code, signal); });

    child_->on_stderr([this](std::string_view line) { on_bridge_line(line); });
    std::unique_lock<std::mutex> lock(bridge_mu_);
    const bool settled = bridge_cv_.wait_for(lock, sandbox.startup_timeout,
                                             [this] { return bridge_ready_ || bridge_failed_; });
    if (!settled) throw McpError{McpError::Kind::handshake, config_.name + ": sandbox bridge did not become ready"};
    if (bridge_failed_) throw McpError{McpError::Kind::handshake,
                                       config_.name + ": sandbox bridge failed: " + bridge_error_};
}

void StdioTransport::on_bridge_line(std::string_view line) {
    const nlohmann::json frame = nlohmann::json::parse(line, nullptr, false);
    if (frame.is_discarded() || !frame.value("dagent_bridge", false)) {
        base::logger("mcp")->info("{}", line); // server 自身的 stderr
        return;
    }
    const std::string type = frame.value("type", "");
    {
        const std::lock_guard lock(bridge_mu_);
        if (type == "ready") {
            const bool ok = frame.value("protocol", 0) == 1 &&
                            frame.value("environment", "") == config_.sandbox->runtime.entry.string() &&
                            frame.value("sandboxing_enabled", false) &&
                            frame.value("srt", "") == exec::kSrtVersion;
            if (ok) bridge_ready_ = true;
            else {
                bridge_failed_ = true;
                bridge_error_ = "SRT protocol, package version or environment identity mismatch";
            }
        } else if (type == "error") {
            bridge_failed_ = true;
            bridge_error_ = frame.value("stage", std::string{"bridge"}) + ": " + frame.value("error", "sandbox bridge failed");
        } else if (type == "exited" && !bridge_ready_) {
            bridge_failed_ = true;
            bridge_error_ = "sandbox bridge exited before ready";
        }
    }
    bridge_cv_.notify_all();
}

void StdioTransport::shutdown(const CloseProtocol& close_protocol) {
    std::unique_ptr<exec::Child> child;
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
    if (child) child->terminate(); // 锁外等读取线程退出，之后不会再回调
    child.reset();
    if (!state_dir_.empty()) {
        std::error_code error;
        std::filesystem::remove_all(state_dir_, error);
        state_dir_.clear();
    }
    if (!private_home_.empty()) {
        std::error_code error;
        std::filesystem::remove_all(private_home_, error);
        private_home_.clear();
    }
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
