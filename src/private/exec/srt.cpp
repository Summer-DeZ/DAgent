#include "exec/srt.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#include "base/log.hpp"
#include "lib/nlohmann/json.hpp"

namespace dagent::exec {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string system_error(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

fs::path make_unique_dir(const fs::path& parent, std::string_view prefix) {
    std::error_code error;
    fs::create_directories(parent, error);
    std::string pattern = (parent / (std::string(prefix) + "-XXXXXX")).string();
    if (::mkdtemp(pattern.data()) == nullptr) throw ExecError{ExecError::Kind::sandbox, system_error("mkdtemp")};
    return pattern;
}

/// 已存在的 literal 去重；SRT 会为缺失的拒绝路径建占位点，这里只传已存在路径。
std::vector<std::string> existing_unique(std::vector<fs::path> paths) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (auto& path : paths) {
        std::error_code error;
        const fs::path canonical = fs::weakly_canonical(path, error);
        if (error || !fs::exists(canonical, error)) continue;
        const std::string text = canonical.string();
        if (seen.insert(text).second) out.push_back(text);
    }
    return out;
}

json build_config(const SrtRequest& request, const fs::path& private_home) {
    const Policy& policy = request.policy;
    std::vector<fs::path> readable = {
        "/bin", "/sbin", "/usr", "/lib", "/lib64", "/etc", "/dev", "/proc", "/sys",
    };
    readable.insert(readable.end(), policy.readable.begin(), policy.readable.end());
    readable.push_back(private_home);
    readable.push_back(request.runtime.entry.parent_path().parent_path()); // SRT 包（apply-seccomp 等）
    readable.push_back(request.runtime.shell.parent_path());
    readable.push_back(request.runtime.rg.parent_path());

    std::vector<fs::path> writable = policy.mode == Mode::read_only ? std::vector<fs::path>{} : policy.writable;
    writable.push_back(private_home);

    std::vector<fs::path> protected_write = policy.protected_write;
    if (policy.protect_sensitive_names) {
        for (const auto& path : writable) {
            if (const auto found = sensitive_paths(path); !found.empty())
                protected_write.insert(protected_write.end(), found.begin(), found.end());
        }
    }

    // 敏感读取：denyRead 里更具体的条目优先于 allowRead 的大范围重开。
    std::vector<fs::path> deny_read = {"/"};
    deny_read.insert(deny_read.end(), policy.protected_read.begin(), policy.protected_read.end());
    if (policy.protect_sensitive_names) {
        for (const auto& path : policy.readable) {
            if (const auto found = sensitive_paths(path); !found.empty())
                deny_read.insert(deny_read.end(), found.begin(), found.end());
        }
    }

    std::erase_if(deny_read, [&](const auto& path) {
        return std::ranges::find(policy.read_exceptions, path) != policy.read_exceptions.end();
    });
    // strictAllowlist=false：未命中目标交给 askCallback（bridge → 宿主判定）；宿主没有审批入口时同样拒绝。
    json network = {{"allowedDomains", policy.network_targets},
                    {"deniedDomains", json::array()},
                    {"strictAllowlist", false}};
    json filesystem = {{"denyRead", existing_unique(deny_read)},
                       {"allowRead", existing_unique(readable)},
                       {"allowWrite", existing_unique(writable)},
                       {"denyWrite", existing_unique(protected_write)}};
    return {{"network", std::move(network)}, {"filesystem", std::move(filesystem)}};
}

struct ExecutionResources {
    fs::path control_dir, private_home;
    int control[2] = {-1, -1};
    void close() {
        for (int& fd : control) {
            if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); fd = -1; }
        }
    }
    ~ExecutionResources() {
        close();
        std::error_code error;
        if (!control_dir.empty()) fs::remove_all(control_dir, error);
        if (!private_home.empty()) fs::remove_all(private_home, error);
    }
};

struct ControlState {
    std::mutex mutex;
    bool exited = false;
    std::optional<int> exit_code;
    std::optional<int> signal;
    bool cancelled = false;
    std::string bridge_error;
    int network_requests = 0;
};

void append_decision(int fd, int request_id, bool allow, bool cancel) {
    const json frame = {{"type", "network_decision"}, {"request_id", request_id}, {"allow", allow},
                        {"cancel", cancel}};
    const std::string line = frame.dump() + "\n";
    const ssize_t written = ::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
    if (written < 0 && errno != EPIPE)
        base::logger("exec")->warn("srt control write failed: {}", std::strerror(errno));
}

} // namespace

Result run_srt(const SrtRequest& request, const Options& options,
               const std::function<void(Stream, std::string_view)>& on_output, std::stop_token stop) {
    ExecutionResources resources;
    auto state_root = request.state_root;
    for (const auto& writable : request.policy.writable) {
        const auto relative = state_root.lexically_relative(writable);
        if (!relative.empty() && *relative.begin() != "..") {
            state_root = fs::temp_directory_path();
            break;
        }
    }
    resources.control_dir = make_unique_dir(state_root, "dagent-run");
    resources.private_home = make_unique_dir(state_root, "dagent-home");
    const auto& control_dir = resources.control_dir;
    const auto& private_home = resources.private_home;
    for (const auto& writable : request.policy.writable) {
        const auto relative = control_dir.lexically_relative(writable);
        if (!relative.empty() && *relative.begin() != "..")
            throw ExecError{ExecError::Kind::sandbox, "unsupported profile: bridge control directory is inside a writable root"};
    }
    std::error_code error;
    fs::create_directories(private_home / "config", error);
    fs::create_directories(private_home / "cache", error);
    fs::create_directories(private_home / "run", error);

    auto& control = resources.control;
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, control) != 0)
        throw ExecError{ExecError::Kind::sandbox, system_error("socketpair")};
    const auto cleanup_fds = [&] { resources.close(); };
    std::stop_source gates_stop;
    std::stop_callback cancel_gates(stop, [&] { gates_stop.request_stop(); });
    std::vector<std::jthread> gates;
    std::mutex control_write;
    const int channel = control[0];

    try {
        // 父端 CLOEXEC；子端保留给 bridge。
        ::fcntl(control[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(control[1], F_SETFD, 0);

        ControlState state;
        json init = {{"type", "init"},
                     {"command", request.command},
                     {"workspace", request.workspace.string()},
                     {"shell", request.runtime.shell.string()},
                     {"config", build_config(request, private_home)},
                     {"network_approval_timeout_ms", request.approval_timeout.count()},
                     {"max_network_requests", request.max_network_requests},
                     {"kill_grace_ms", options.kill_grace.count()}};
        const std::string init_line = init.dump() + "\n";
        if (::write(control[0], init_line.data(), init_line.size()) < 0)
            throw ExecError{ExecError::Kind::sandbox, system_error("control init write")};

        std::jthread reader([&](std::stop_token reader_stop) {
            std::string buffer;
            char chunk[4096];
            while (!reader_stop.stop_requested()) {
                const ssize_t bytes = ::read(channel, chunk, sizeof(chunk));
                if (bytes <= 0) break;
                buffer.append(chunk, static_cast<std::size_t>(bytes));
                for (;;) {
                    const std::size_t newline = buffer.find('\n');
                    if (newline == std::string::npos) break;
                    const std::string line = buffer.substr(0, newline);
                    buffer.erase(0, newline + 1);
                    json frame;
                    try {
                        frame = json::parse(line);
                    } catch (...) {
                        continue;
                    }
                    const std::string type = frame.value("type", "");
                    if (type == "network_request") {
                        {
                            const std::lock_guard lock(state.mutex);
                            state.network_requests += 1;
                        }
                        gates.emplace_back([&, frame] {
                            std::string reason;
                            NetworkGateResult action = NetworkGateResult::deny;
                            try {
                                if (request.network_gate)
                                    action = request.network_gate(frame.value("host", ""), frame.value("port", 0),
                                                                  reason, gates_stop.get_token());
                            } catch (...) { action = NetworkGateResult::cancel; }
                            const std::lock_guard lock(control_write);
                            if (!gates_stop.stop_requested())
                                append_decision(channel, frame.value("request_id", 0),
                                                action == NetworkGateResult::allow,
                                                action == NetworkGateResult::cancel);
                        });
                    } else if (type == "ready") {
                        if (frame.value("protocol", 0) != 1 || frame.value("srt", "") != "0.0.77" ||
                            frame.value("environment", "") != request.runtime.entry.string()) {
                            const std::lock_guard lock(state.mutex);
                            state.bridge_error = "SRT protocol, package version or environment identity mismatch";
                            ::shutdown(channel, SHUT_RDWR);
                        } else {
                            const std::lock_guard lock(control_write);
                            const std::string start = "{\"type\":\"start\"}\n";
                            ::send(channel, start.data(), start.size(), MSG_NOSIGNAL);
                        }
                    } else if (type == "network_expired") {
                        gates_stop.request_stop();
                    } else if (type == "exited") {
                        gates_stop.request_stop();
                        const std::lock_guard lock(state.mutex);
                        state.exited = true;
                        state.cancelled = frame.value("cancelled", false);
                        if (!frame["exit_code"].is_null()) state.exit_code = frame["exit_code"].get<int>();
                        if (!frame["signal"].is_null()) state.signal = frame["signal"].get<int>();
                    } else if (type == "bridge_error") {
                        const std::lock_guard lock(state.mutex);
                        state.bridge_error = frame.value("error", "bridge failed");
                    }
                }
            }
        });

        Command cmd;
        cmd.argv = {request.runtime.node.string(), request.runtime.bridge.string(), "--mode=run",
                    "--srt=" + request.runtime.entry.string(), "--bwrap=" + request.runtime.bwrap.string(),
                    "--socat=" + request.runtime.socat.string(), "--rg=" + request.runtime.rg.string(),
                    "--shell=" + request.runtime.shell.string(),
                    "--control-fd=" + std::to_string(control[1])};
        cmd.cwd = control_dir;
        cmd.timeout = request.timeout;
        cmd.inherit_env = false;
        cmd.env_set = request.environment;
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

        Result result;
        try {
            result = run(cmd, options, on_output, stop);
        } catch (...) {
            gates_stop.request_stop();
            reader.request_stop();
            ::shutdown(channel, SHUT_RDWR);
            reader.join();
            gates.clear();
            cleanup_fds();
            throw;
        }
        gates_stop.request_stop();
        ::close(control[1]);
        control[1] = -1;
        reader.join(); // Drain the structured exit frame before releasing the control socket.
        gates.clear();
        cleanup_fds();

        const std::lock_guard lock(state.mutex);
        if (!state.bridge_error.empty())
            throw ExecError{ExecError::Kind::sandbox, "sandbox bridge failed: " + state.bridge_error};
        if (state.exited) {
            result.exit_code = state.exit_code;
            result.signal = state.signal;
            if (state.cancelled)
                throw ExecError{ExecError::Kind::cancelled, "command cancelled by a runtime permission decision"};
        } else if (!result.timed_out && result.exit_code && *result.exit_code != 0) {
            const std::string detail = result.err.empty() ? result.out : result.err;
            throw ExecError{ExecError::Kind::sandbox,
                            "sandbox bridge exited without a result: " + detail.substr(0, 500)};
        }
        return result;
    } catch (...) {
        cleanup_fds();
        throw;
    }
}

Result ReadOnlySandbox::run(const Command& command, const Options& options,
                            const std::function<void(Stream, std::string_view)>& output,
                            std::stop_token stop) const {
    if (!runtime) throw ExecError{ExecError::Kind::sandbox, "read-only SRT execution unavailable; run dagent sandbox status"};
    SrtRequest request;
    request.runtime = *runtime;
    request.workspace = command.cwd;
    request.state_root = state_root;
    request.timeout = command.timeout;
    request.policy.mode = Mode::read_only;
    request.policy.readable = {command.cwd};
    request.policy.protected_read = protected_read;
    request.policy.read_exceptions = read_exceptions;
    request.environment = command.env_set;
    request.environment.emplace_back("PATH", "/usr/bin:/bin");
    request.environment.emplace_back("LC_ALL", "C.UTF-8");
    for (const auto& argument : command.argv) {
        if (!request.command.empty()) request.command += ' ';
        request.command += "'";
        for (char c : argument) request.command += c == '\'' ? "'\\''" : std::string(1, c);
        request.command += "'";
    }
    return run_srt(request, options, output, stop);
}

} // namespace dagent::exec
