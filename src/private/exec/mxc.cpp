#include "exec/mxc.hpp"

#include "base/log.hpp"
#include "base/path.hpp"
#include "base/platform.hpp"
#include "exec/detail.hpp"
#include "exec/platform.hpp"
#include "exec/proxy.hpp"

#include <algorithm>
#include <array>
#include <future>
#include <atomic>
#include <bit>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <format>
#include <mutex>
#include <set>
#include <thread>

namespace dagent::exec {

using json = nlohmann::json;
namespace fs = std::filesystem;

struct MxcLibrary::Impl {
    fs::path file;
    detail::SharedLibrary library;
    ffi::Functions functions{};
};

namespace {

using namespace std::chrono_literals;

std::string take_string(const ffi::Functions& f, char* text) {
    if (text == nullptr) return {};
    std::string result(text);
    f.string_free(text);
    return result;
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// 已存在的路径按 weakly_canonical 去重；MXC 只拿到已存在的路径，不制造占位点。
std::vector<fs::path> existing_unique(const std::vector<fs::path>& paths) {
    std::vector<fs::path> out;
    std::set<fs::path> seen;
    for (const auto& path : paths) {
        std::error_code error;
        fs::path canonical = fs::weakly_canonical(path, error);
        if (error || !fs::exists(canonical, error)) continue;
        if (seen.insert(canonical).second) out.push_back(std::move(canonical));
    }
    return out;
}

bool within(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

json path_array(const std::vector<fs::path>& paths) {
    json out = json::array();
    for (const auto& path : paths) out.push_back(base::path_to_utf8(path));
    return out;
}

/// 释放 FFI 错误详情并给出消息（含 remediation）；没有消息时按 operation 与状态码描述。
std::string error_message(const ffi::Functions& f, ffi::MxcErrorDetail& detail, std::string_view operation,
                          std::int32_t status) {
    std::string message = detail.message_utf8 != nullptr ? detail.message_utf8 : "";
    if (detail.remediation_utf8 != nullptr) message += std::format(" ({})", detail.remediation_utf8);
    f.error_detail_free(&detail);
    return message.empty() ? std::format("{} failed with status {}", operation, status) : "sandbox: " + message;
}

/// 命令环境：调用方变量之上把 HOME/XDG/TMPDIR 系列与平台变量指向私有 HOME，并清掉会改变 shell 或
/// 代理行为的变量（代理由 MXC 按 runtimeConfig 设置）。
std::vector<std::string> environment(const MxcRequest& request, const Options& options, const fs::path& home) {
    Command cmd;
    cmd.inherit_env = false;
    cmd.env_set = request.environment;
    cmd.env_set.insert(cmd.env_set.end(),
                       {{"HOME", base::path_to_utf8(home)},
                        {"XDG_CONFIG_HOME", base::path_to_utf8(home / "config")},
                        {"XDG_CACHE_HOME", base::path_to_utf8(home / "cache")},
                        // 托管环境把包缓存指向 Home，沙箱内不可见也不可写；与 XDG 缓存一样落到私有 HOME。
                        {"UV_CACHE_DIR", base::path_to_utf8(home / "cache" / "uv")},
                        {"NPM_CONFIG_CACHE", base::path_to_utf8(home / "cache" / "npm")},
                        {"XDG_RUNTIME_DIR", base::path_to_utf8(home / "run")},
                        {"TMPDIR", base::path_to_utf8(home)},
                        {"TMP", base::path_to_utf8(home)},
                        {"TEMP", base::path_to_utf8(home)}});
    const auto platform = detail::sandbox_environment(home);
    cmd.env_set.insert(cmd.env_set.end(), platform.begin(), platform.end());
    cmd.env_unset = {"BASH_ENV", "ENV", "BASHOPTS", "SHELLOPTS", "CDPATH", "GLOBIGNORE",
                     "PROMPT_COMMAND", "LD_PRELOAD", "LD_LIBRARY_PATH", "PYTHONPATH", "PERL5LIB",
                     "RUBYOPT", "NODE_OPTIONS", "NODE_PATH", "NODE_EXTRA_CA_CERTS",
                     "http_proxy", "https_proxy", "all_proxy", "ftp_proxy",
                     "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "FTP_PROXY"};
    return detail::make_environment(cmd, options);
}

/// 一次受限执行提交给 MXC 的完整请求：策略加 process（managed bash 执行脚本，超时由本进程落实）。
json request_document(const MxcRequest& request, const Options& options, const fs::path& home,
                      std::optional<std::string> proxy_url, bool keep_stdin) {
    json document = mxc_policy(request.runtime, request.policy, home, std::move(proxy_url));
    // MXC 总是给沙箱一个 stdin 管道；不保留 stdin 时改接 /dev/null，否则 rg 这类工具会把可读的空管道当成搜索输入。
    const std::string script = std::string(keep_stdin ? "" : "exec </dev/null\n") +
                               (request.merge_stderr ? "exec 2>&1\n" : "") + request.command;
    document["process"] = {{"commandLine", detail::sandbox_command_line(request.runtime.shell, script)},
                           {"cwd", base::path_to_utf8(request.workspace)},
                           {"env", environment(request, options, home)},
                           {"timeout", 0}};
    return document;
}

struct Exit {
    std::optional<int> code;
    bool timed_out = false;
};

/// 一次 MXC 执行持有的资源：私有 HOME、回环代理、沙箱句柄与三条流。
/// 同一句柄上的调用经 mutex_ 串行化（FFI 并发契约）；两条读取线程各自驱动一条输出流。
/// release() 先终止仍在运行的进程树，再按 流 → 沙箱 → 代理 → 私有 HOME 的顺序清理，可重复调用。
class Execution {
public:
    using Sink = std::function<void(Stream, std::string_view)>;
    using Closed = std::function<void(Stream)>;

    explicit Execution(std::shared_ptr<const MxcLibrary> library)
        : library_(std::move(library)), f_(library_->functions()) {}
    ~Execution() { release(); }
    Execution(const Execution&) = delete;
    Execution& operator=(const Execution&) = delete;

    /// 失败抛 ExecError{sandbox}；已创建的资源由 release() 回收。
    void start(const MxcRequest& request, const Options& options, bool keep_stdin, Sink sink, Closed closed) {
        sink_ = std::move(sink);
        closed_ = std::move(closed);

        auto directory = base::create_unique_directory(base::default_temporary_directory() / "dagent-home-XXXXXX");
        if (!directory) throw ExecError{ExecError::Kind::sandbox, "mkdtemp: " + directory.error()};
        home_ = *directory;
        std::error_code error;
        fs::create_directories(home_ / "config", error);
        fs::create_directories(home_ / "cache", error);
        fs::create_directories(home_ / "run", error);

        if (request.network_gate) {
            proxy_ = std::make_unique<NetworkProxy>(
                allowlist_gate(request.policy.network_targets, request.network_gate),
                NetworkProxy::Limits{request.approval_timeout, request.max_network_requests}, [this] {
                    gate_cancelled_ = true;
                    kill();
                });
        }

        const std::string text =
            request_document(request, options, home_, proxy_ ? std::optional(proxy_->url()) : std::nullopt, keep_stdin)
                .dump(-1, ' ', false, json::error_handler_t::replace);

        // bwrap 以 --die-with-parent（PDEATHSIG）跟随创建它的线程：spawn 必须发生在活到进程结束的线程上，
        // 所以由 stdout 读取线程负责 spawn，再继续读 stdout；调用线程只等待 spawn 结果。
        std::promise<void> spawned;
        std::future<void> ready = spawned.get_future();
        readers_[0] = std::thread([this, text, keep_stdin, promise = std::move(spawned)]() mutable {
            try {
                spawn(text, keep_stdin);
                promise.set_value();
            } catch (...) {
                promise.set_exception(std::current_exception());
                return;
            }
            if (streams_[0] == nullptr) closed_(Stream::out);
            else read_loop(streams_[0], Stream::out);
        });
        try {
            ready.get();
        } catch (...) {
            readers_[0].join();
            throw;
        }
        if (streams_[1] == nullptr) closed_(Stream::err);
        else readers_[1] = std::thread([this] { read_loop(streams_[1], Stream::err); });
    }

    void spawn(const std::string& text, bool keep_stdin) {
        ffi::MxcErrorDetail error_detail{};
        const std::int32_t status = f_.spawn_json(text.c_str(), 0, &sandbox_, &error_detail);
        const std::string message = error_message(f_, error_detail, "mxc_spawn_json", status);
        if (status != ffi::ok || sandbox_ == nullptr) {
            sandbox_ = nullptr;
            throw ExecError{ExecError::Kind::sandbox, message};
        }
        running_ = true;

        char* warnings = nullptr;
        if (f_.sandbox_warnings_json(sandbox_, &warnings) == ffi::ok)
            if (const std::string warning_text = take_string(f_, warnings); !warning_text.empty())
                base::logger("exec")->debug("mxc warnings: {}", warning_text);

        input_ = f_.sandbox_take_stdin(sandbox_);
        if (!keep_stdin && input_ != nullptr) {
            f_.write_stream_free(input_); // 不给 stdin 时立即送出 EOF，等同 /dev/null
            input_ = nullptr;
        }
        streams_[0] = f_.sandbox_take_stdout(sandbox_);
        streams_[1] = f_.sandbox_take_stderr(sandbox_);
        closers_[0] = f_.sandbox_stdout_closer(sandbox_);
        closers_[1] = f_.sandbox_stderr_closer(sandbox_);
    }

    /// 仍在运行时返回空；退出后反复调用返回同一结果。
    std::optional<Exit> try_wait() {
        const std::lock_guard lock(mutex_);
        if (!running_) return exit_;
        std::int32_t code = 0, running = 0, timed_out = 0;
        if (f_.sandbox_try_wait(sandbox_, &code, &running, &timed_out) != ffi::ok) {
            base::logger("exec")->warn("mxc_sandbox_try_wait failed");
            running_ = false;
            exit_ = Exit{};
            return exit_;
        }
        if (running != 0) return std::nullopt;
        running_ = false;
        exit_ = Exit{code, timed_out != 0};
        return exit_;
    }

    void kill(bool for_timeout = false) {
        const std::lock_guard lock(mutex_);
        if (sandbox_ == nullptr || !running_) return;
        (for_timeout ? f_.sandbox_kill_for_timeout : f_.sandbox_kill)(sandbox_);
    }

    /// 写 stdin（只在 keep_stdin 时可用）；失败返回 false，之后不应再写。
    bool write(std::string_view data) {
        while (!data.empty()) {
            std::size_t written = 0;
            if (f_.stream_write(input_, reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), &written) !=
                    ffi::ok ||
                written == 0)
                return false;
            data.remove_prefix(written);
        }
        return f_.stream_flush(input_) == ffi::ok;
    }

    bool gate_cancelled() const noexcept { return gate_cancelled_; }

    void release() noexcept {
        kill();
        for (std::size_t i = 0; i < 2; ++i) {
            if (closers_[i] != nullptr) f_.stream_closer_close(closers_[i]); // 仍阻塞的读取以 EOF 返回
            if (readers_[i].joinable()) readers_[i].join();
            if (streams_[i] != nullptr) f_.read_stream_free(streams_[i]);
            if (closers_[i] != nullptr) f_.stream_closer_free(closers_[i]);
            streams_[i] = nullptr;
            closers_[i] = nullptr;
        }
        if (input_ != nullptr) f_.write_stream_free(input_);
        input_ = nullptr;
        {
            const std::lock_guard lock(mutex_);
            if (sandbox_ != nullptr) {
                if (running_) {
                    std::int32_t code = 0, timed_out = 0;
                    f_.sandbox_wait(sandbox_, &code, &timed_out);
                    running_ = false;
                }
                f_.sandbox_free(sandbox_);
                sandbox_ = nullptr;
            }
        }
        if (proxy_) proxy_->stop();
        proxy_.reset();
        if (!home_.empty()) {
            std::error_code error;
            fs::remove_all(home_, error);
            home_.clear();
        }
    }

private:
    void read_loop(ffi::MxcReadStream* stream, Stream which) {
        std::array<std::uint8_t, 64 << 10> buffer;
        for (;;) {
            std::size_t size = 0;
            if (f_.stream_read(stream, buffer.data(), buffer.size(), &size) != ffi::ok || size == 0) break;
            sink_(which, std::string_view(reinterpret_cast<const char*>(buffer.data()), size));
        }
        closed_(which);
    }

    std::shared_ptr<const MxcLibrary> library_;
    const ffi::Functions& f_;
    Sink sink_;
    Closed closed_;
    fs::path home_;
    std::unique_ptr<NetworkProxy> proxy_;
    std::mutex mutex_;
    ffi::MxcSandbox* sandbox_ = nullptr;
    bool running_ = false;
    std::optional<Exit> exit_;
    ffi::MxcWriteStream* input_ = nullptr;
    std::array<ffi::MxcReadStream*, 2> streams_{};
    std::array<ffi::MxcStreamCloser*, 2> closers_{};
    std::array<std::thread, 2> readers_;
    std::atomic<bool> gate_cancelled_{false};
};

} // namespace

MxcLibrary::MxcLibrary() = default;
MxcLibrary::~MxcLibrary() = default;

std::shared_ptr<const MxcLibrary> MxcLibrary::load(const fs::path& file) {
    auto library = std::shared_ptr<MxcLibrary>(new MxcLibrary());
    library->impl_ = std::make_unique<Impl>(Impl{file, detail::SharedLibrary::open(file), {}});
    const auto& shared = library->impl_->library;
    auto& f = library->impl_->functions;
    const auto resolve = [&]<typename T>(T& slot, const char* name) {
        void* address = shared.symbol(name);
        if (address == nullptr)
            throw ExecError{ExecError::Kind::sandbox, std::format("load {}: missing symbol {}", base::path_to_utf8(file), name)};
        slot = std::bit_cast<T>(address);
    };
    resolve(f.version, "mxc_version");
    resolve(f.platform_support_json, "mxc_platform_support_json");
    resolve(f.available_backends_json, "mxc_available_backends_json");
    resolve(f.string_free, "mxc_string_free");
    resolve(f.error_detail_free, "mxc_error_detail_free");
    resolve(f.run_json, "mxc_run_json");
    resolve(f.run_result_free, "mxc_run_result_free");
    resolve(f.spawn_json, "mxc_spawn_json");
    resolve(f.sandbox_take_stdin, "mxc_sandbox_take_stdin");
    resolve(f.sandbox_take_stdout, "mxc_sandbox_take_stdout");
    resolve(f.sandbox_take_stderr, "mxc_sandbox_take_stderr");
    resolve(f.sandbox_stdout_closer, "mxc_sandbox_stdout_closer");
    resolve(f.sandbox_stderr_closer, "mxc_sandbox_stderr_closer");
    resolve(f.stream_read, "mxc_stream_read");
    resolve(f.stream_write, "mxc_stream_write");
    resolve(f.stream_flush, "mxc_stream_flush");
    resolve(f.stream_closer_close, "mxc_stream_closer_close");
    resolve(f.stream_closer_free, "mxc_stream_closer_free");
    resolve(f.read_stream_free, "mxc_read_stream_free");
    resolve(f.write_stream_free, "mxc_write_stream_free");
    resolve(f.sandbox_id, "mxc_sandbox_id");
    resolve(f.sandbox_try_wait, "mxc_sandbox_try_wait");
    resolve(f.sandbox_wait, "mxc_sandbox_wait");
    resolve(f.sandbox_kill, "mxc_sandbox_kill");
    resolve(f.sandbox_kill_for_timeout, "mxc_sandbox_kill_for_timeout");
    resolve(f.sandbox_free, "mxc_sandbox_free");
    resolve(f.sandbox_warnings_json, "mxc_sandbox_warnings_json");
    f.probe_request_json_with_error = std::bit_cast<decltype(f.probe_request_json_with_error)>(
        shared.symbol("mxc_probe_request_json_with_error"));
    if (const std::string version = f.version(); version != kMxcVersion)
        throw ExecError{ExecError::Kind::sandbox,
                        std::format("load {}: MXC version {} does not match the supported {}", base::path_to_utf8(file), version,
                                    kMxcVersion)};
    return library;
}

const fs::path& MxcLibrary::file() const noexcept { return impl_->file; }

std::string MxcLibrary::version() const { return impl_->functions.version(); }

json MxcLibrary::platform_support() const {
    return json::parse(take_string(impl_->functions, impl_->functions.platform_support_json()));
}

json MxcLibrary::available_backends() const {
    return json::parse(take_string(impl_->functions, impl_->functions.available_backends_json()));
}

std::optional<json> MxcLibrary::probe_request(const json& request) const {
    const auto& f = impl_->functions;
    if (f.probe_request_json_with_error == nullptr) return std::nullopt;
    const std::string text = request.dump(-1, ' ', false, json::error_handler_t::replace);
    char* output = nullptr;
    ffi::MxcErrorDetail error_detail{};
    const std::int32_t status = f.probe_request_json_with_error(text.c_str(), &output, &error_detail);
    const std::string message = error_message(f, error_detail, "mxc_probe_request_json", status);
    if (status != ffi::ok || output == nullptr) throw ExecError{ExecError::Kind::sandbox, message};
    return json::parse(take_string(f, output));
}

const ffi::Functions& MxcLibrary::functions() const noexcept { return impl_->functions; }

json mxc_policy(const MxcRuntime& runtime, const Policy& policy, const fs::path& private_home,
                std::optional<std::string> proxy_url) {
    std::vector<fs::path> writable_input = policy.mode == Mode::read_only ? std::vector<fs::path>{} : policy.writable;
    writable_input.push_back(private_home);
    const auto writable = existing_unique(writable_input);
    const auto inside_writable = [&](const fs::path& path) {
        return std::ranges::any_of(writable, [&](const fs::path& root) { return within(path, root); });
    };

    std::vector<fs::path> readable_input = policy.readable;
    readable_input.push_back(runtime.shell.parent_path());
    readable_input.push_back(runtime.rg.parent_path());
    // 同一路径同时出现在只读与读写授权里时 MXC 以只读为准；可写树内的读取由读写挂载提供。
    auto readonly = existing_unique(readable_input);
    std::erase_if(readonly, inside_writable);

    std::vector<fs::path> protected_write = policy.protected_write;
    std::vector<fs::path> denied_input = policy.protected_read;
    if (policy.protect_sensitive_names) {
        for (const auto& root : writable) {
            const auto found = sensitive_paths(root);
            protected_write.insert(protected_write.end(), found.begin(), found.end());
        }
        for (const auto& root : policy.readable) {
            const auto found = sensitive_paths(root);
            denied_input.insert(denied_input.end(), found.begin(), found.end());
        }
    }
    // 写保护以嵌套在可写路径内的只读挂载落实；可写树外的条目本就不可写，不借此授予读取。
    for (auto& path : existing_unique(protected_write))
        if (inside_writable(path) && std::ranges::find(readonly, path) == readonly.end())
            readonly.push_back(std::move(path));

    auto denied = existing_unique(denied_input);
    const auto exceptions = existing_unique(policy.read_exceptions);
    std::erase_if(denied, [&](const fs::path& path) { return std::ranges::find(exceptions, path) != exceptions.end(); });

    json document = {{"version", std::string(kMxcPolicyVersion)},
                     {"containment", runtime.containment},
                     {"filesystem",
                      {{"readonlyPaths", path_array(readonly)},
                       {"readwritePaths", path_array(writable)},
                       {"deniedPaths", path_array(denied)}}}};
    const bool process_container = runtime.containment == "processcontainer";
    if (process_container) document["fallback"] = {{"allowDaclMutation", false}};
    if (proxy_url) {
        // ProcessContainer 只在 privateNetworkClientServer（ingress.default=allow）下连得到回环代理，
        // 没有包身份的代理还要 hostLoopback=allow；两者都不放宽 WFP 对代理端点以外出站的拦截。
        const char* ingress = process_container ? "allow" : "deny";
        document["network"] = {{"egress", {{"default", "deny"}}},
                               {"ingress", {{"default", ingress}, {"hostLoopback", ingress}}}};
        document["runtimeConfig"] = {{"networkProxy", *proxy_url}};
    }
    return document;
}

std::optional<json> probe_mxc(const MxcRequest& request, const Options& options) {
    return request.runtime.library->probe_request(
        request_document(request, options, base::default_temporary_directory(),
                         request.network_gate ? std::optional<std::string>("http://127.0.0.1:9") : std::nullopt, false));
}

NetworkGate allowlist_gate(std::vector<std::string> targets, NetworkGate fallback) {
    for (auto& target : targets) target = lower(target);
    return [targets = std::move(targets), fallback = std::move(fallback)](
               std::string_view host, int port, std::string& reason, std::stop_token stop) {
        const std::string name = lower(host);
        const std::string with_port = name.find(':') != std::string::npos ? std::format("[{}]:{}", name, port)
                                                                          : std::format("{}:{}", name, port);
        if (std::ranges::any_of(targets, [&](const std::string& target) { return target == name || target == with_port; }))
            return NetworkGateResult::allow;
        if (fallback) return fallback(host, port, reason, std::move(stop));
        reason = "target is not in the network allowlist";
        return NetworkGateResult::deny;
    };
}

Result run_mxc(const MxcRequest& request, const Options& options,
               const std::function<void(Stream, std::string_view)>& on_output, std::stop_token stop) {
    if (stop.stop_requested()) throw ExecError{ExecError::Kind::cancelled, "command cancelled"};
    detail::ignore_sigpipe();
    const auto start = std::chrono::steady_clock::now();

    struct Chunk {
        Stream stream;
        std::string data;
    };
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Chunk> chunks;
    int open_streams = 2;
    bool cancel_requested = false;

    Execution execution(request.runtime.library);
    execution.start(
        request, options, false,
        [&](Stream stream, std::string_view data) {
            // stderr 已在脚本里并入 stdout；这里剩下的只有 shell 启动前后端自己的诊断，同样归入 stdout。
            const std::lock_guard lock(mutex);
            chunks.push_back({request.merge_stderr ? Stream::out : stream, std::string(data)});
            cv.notify_one();
        },
        [&](Stream) {
            const std::lock_guard lock(mutex);
            --open_streams;
            cv.notify_one();
        });

    std::stop_callback on_stop(stop, [&] {
        const std::lock_guard lock(mutex);
        cancel_requested = true;
        cv.notify_one();
    });

    const auto timeout = request.timeout.value_or(options.default_timeout);
    detail::OutputCollector out(options.max_output_bytes);
    detail::OutputCollector err(options.max_output_bytes);
    std::exception_ptr user_error;
    std::optional<Exit> exit;
    std::chrono::steady_clock::time_point drain_deadline;
    bool killed = false;
    bool timed_out = false;
    bool cancelled = false;

    // 调用线程每 ~10 ms 轮询 try_wait，按到达顺序派发输出，并在这里落实超时与取消。
    for (;;) {
        std::deque<Chunk> ready;
        int open = 0;
        {
            std::unique_lock lock(mutex);
            cv.wait_for(lock, 10ms, [&] { return !chunks.empty() || (cancel_requested && !cancelled); });
            ready.swap(chunks);
            open = open_streams;
            if (cancel_requested) cancelled = true;
        }
        for (const auto& chunk : ready) {
            (chunk.stream == Stream::out ? out : err).append(chunk.data);
            if (!on_output || user_error) continue;
            try {
                on_output(chunk.stream, chunk.data);
            } catch (...) {
                user_error = std::current_exception();
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (!killed && (cancelled || user_error || execution.gate_cancelled())) {
            execution.kill();
            killed = true;
        }
        if (!exit) {
            exit = execution.try_wait();
            if (exit) {
                timed_out = timed_out || exit->timed_out;
                drain_deadline = now + options.drain_after_exit;
            } else if (!killed && timeout.count() > 0 && now - start >= timeout) {
                execution.kill(true);
                killed = true;
                timed_out = true;
            }
        }
        if (exit && (open == 0 || now >= drain_deadline)) break;
    }
    execution.release();

    if (user_error) std::rethrow_exception(user_error);
    if (cancelled) throw ExecError{ExecError::Kind::cancelled, "command cancelled"};
    if (execution.gate_cancelled())
        throw ExecError{ExecError::Kind::cancelled, "command cancelled by a runtime permission decision"};

    Result result;
    if (!timed_out) result.exit_code = exit->code;
    result.timed_out = timed_out;
    result.out = out.finish();
    result.err = err.finish();
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    return result;
}

struct MxcChild::Impl {
    using LineCallback = std::function<void(std::string_view)>;
    using ExitCallback = std::function<void(std::optional<int>, std::optional<int>)>;

    static constexpr std::size_t kMaxPendingBytes = 8 << 20;
    static constexpr std::size_t kMaxStderrBacklog = 256;

    Impl(const MxcRequest& request, const Options& options) : options_(options), execution_(request.runtime.library) {
        detail::ignore_sigpipe(); // server 挂掉后再写 stdin 应得到 EPIPE，而不是杀掉 agent
        execution_.start(
            request, options, true, [this](Stream stream, std::string_view data) { receive(stream, data); },
            [this](Stream stream) { closed(stream); });
        writer_ = std::thread([this] { write_loop(); });
    }

    ~Impl() { terminate(); }

    void write(std::string_view data) {
        const std::lock_guard lock(mutex_);
        if (exited_ || input_closed_) return;
        if (pending_bytes_ + data.size() > kMaxPendingBytes) {
            base::logger("exec")->error("child stdin backlog exceeds {} bytes, dropping write", kMaxPendingBytes);
            return;
        }
        pending_bytes_ += data.size();
        writes_.emplace_back(data);
        write_cv_.notify_one();
    }

    void set_on_line(LineCallback cb) {
        const std::lock_guard lock(mutex_);
        on_line_ = std::move(cb);
    }

    void set_on_stderr(LineCallback cb) {
        std::vector<std::string> backlog;
        {
            const std::lock_guard lock(mutex_);
            on_stderr_ = std::move(cb);
            backlog.assign(stderr_backlog_.begin(), stderr_backlog_.end());
            stderr_backlog_.clear();
        }
        for (const std::string& line : backlog) deliver_stderr(line);
    }

    void set_on_exit(ExitCallback cb) {
        ExitCallback deliver;
        std::optional<int> code;
        {
            const std::lock_guard lock(mutex_);
            on_exit_ = std::move(cb);
            if (exited_ && !exit_delivered_ && on_exit_) {
                exit_delivered_ = true;
                deliver = on_exit_;
                code = exit_code_;
            }
        }
        if (deliver) invoke_exit(deliver, code);
    }

    void terminate() {
        const std::lock_guard life(life_mutex_);
        if (terminated_) return;
        terminated_ = true;
        execution_.kill();
        {
            const std::lock_guard lock(mutex_);
            input_closed_ = true;
            write_cv_.notify_all();
        }
        if (writer_.joinable()) writer_.join();
        execution_.release();
    }

private:
    void write_loop() {
        for (;;) {
            std::string chunk;
            {
                std::unique_lock lock(mutex_);
                write_cv_.wait(lock, [&] { return input_closed_ || !writes_.empty(); });
                if (input_closed_) return;
                chunk = std::move(writes_.front());
                writes_.pop_front();
            }
            const bool ok = execution_.write(chunk);
            const std::lock_guard lock(mutex_);
            pending_bytes_ -= chunk.size();
            if (!ok) {
                base::logger("exec")->debug("write child stdin failed; dropping further writes");
                input_closed_ = true;
                writes_.clear();
                pending_bytes_ = 0;
                return;
            }
        }
    }

    // 每条输出流只由自己的读取线程访问对应的半行缓冲。
    void receive(Stream stream, std::string_view data) {
        std::string& partial = stream == Stream::out ? out_partial_ : err_partial_;
        partial.append(data);
        std::size_t begin = 0;
        for (std::size_t end; (end = partial.find('\n', begin)) != std::string::npos; begin = end + 1)
            dispatch(stream, partial.substr(begin, end - begin));
        partial.erase(0, begin);
    }

    void dispatch(Stream stream, std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (stream == Stream::out) deliver_line(line);
        else log_line(std::move(line));
    }

    void closed(Stream stream) {
        std::string& partial = stream == Stream::out ? out_partial_ : err_partial_;
        if (!partial.empty()) dispatch(stream, std::exchange(partial, {}));
        if (stream == Stream::err) {
            const std::lock_guard lock(mutex_);
            err_closed_ = true;
            exit_cv_.notify_all();
            return;
        }
        // stdout 结束即进程结束（沙箱的 PID 命名空间随首进程退出），轮询拿到退出码后再等 stderr 收尾。
        std::optional<Exit> exit;
        while (!(exit = execution_.try_wait())) std::this_thread::sleep_for(10ms);
        ExitCallback deliver;
        {
            std::unique_lock lock(mutex_);
            exit_cv_.wait_for(lock, options_.drain_after_exit, [&] { return err_closed_; });
            exited_ = true;
            exit_code_ = exit->code;
            input_closed_ = true;
            write_cv_.notify_all();
            if (on_exit_ && !exit_delivered_) {
                exit_delivered_ = true;
                deliver = on_exit_;
            }
        }
        if (deliver) invoke_exit(deliver, exit->code);
    }

    void deliver_line(const std::string& line) {
        LineCallback cb;
        {
            const std::lock_guard lock(mutex_);
            cb = on_line_;
        }
        if (!cb) return;
        try {
            cb(line);
        } catch (const std::exception& e) {
            base::logger("exec")->error("child on_line callback threw: {}", e.what());
        }
    }

    void log_line(std::string line) {
        {
            const std::lock_guard lock(mutex_);
            if (!on_stderr_) {
                if (stderr_backlog_.size() >= kMaxStderrBacklog) stderr_backlog_.pop_front();
                stderr_backlog_.push_back(line);
                base::logger("mcp")->info("{}", line);
                return;
            }
        }
        deliver_stderr(line); // 有回调时在锁外派发，避免回调再进入 MxcChild 时自锁
    }

    void deliver_stderr(const std::string& line) {
        LineCallback cb;
        {
            const std::lock_guard lock(mutex_);
            cb = on_stderr_;
        }
        if (!cb) {
            base::logger("mcp")->info("{}", line);
            return;
        }
        try {
            cb(line);
        } catch (const std::exception& e) {
            base::logger("exec")->error("child on_stderr callback threw: {}", e.what());
        }
    }

    void invoke_exit(const ExitCallback& cb, std::optional<int> code) {
        try {
            cb(code, std::nullopt);
        } catch (const std::exception& e) {
            base::logger("exec")->error("child on_exit callback threw: {}", e.what());
        }
    }

    Options options_;
    std::mutex mutex_;
    std::mutex life_mutex_;
    std::condition_variable write_cv_;
    std::condition_variable exit_cv_;
    LineCallback on_line_;
    LineCallback on_stderr_;
    std::deque<std::string> stderr_backlog_;
    ExitCallback on_exit_;
    std::deque<std::string> writes_;
    std::size_t pending_bytes_ = 0;
    std::string out_partial_;
    std::string err_partial_;
    bool input_closed_ = false;
    bool err_closed_ = false;
    bool exited_ = false;
    bool exit_delivered_ = false;
    bool terminated_ = false;
    std::optional<int> exit_code_;
    std::thread writer_;
    Execution execution_; // 最后构造、最先析构：读取线程回调会访问上面的成员
};

MxcChild::MxcChild() = default;
MxcChild::~MxcChild() = default;

std::unique_ptr<MxcChild> MxcChild::spawn(const MxcRequest& request, const Options& options) {
    auto child = std::unique_ptr<MxcChild>(new MxcChild());
    child->impl_ = std::make_unique<Impl>(request, options);
    return child;
}

void MxcChild::write(std::string_view data) { impl_->write(data); }

void MxcChild::on_line(std::function<void(std::string_view)> cb) { impl_->set_on_line(std::move(cb)); }

void MxcChild::on_stderr(std::function<void(std::string_view)> cb) { impl_->set_on_stderr(std::move(cb)); }

void MxcChild::on_exit(std::function<void(std::optional<int>, std::optional<int>)> cb) {
    impl_->set_on_exit(std::move(cb));
}

void MxcChild::terminate() { impl_->terminate(); }

Result ReadOnlySandbox::run(const Command& command, const Options& options,
                            const std::function<void(Stream, std::string_view)>& output, std::stop_token stop) const {
    if (!runtime) throw ExecError{ExecError::Kind::sandbox, "read-only sandbox execution unavailable; run dagent sandbox status"};
    MxcRequest request;
    request.runtime = *runtime;
    request.workspace = command.cwd;
    request.timeout = command.timeout;
    request.policy.mode = Mode::read_only;
    request.policy.readable = {command.cwd};
    request.policy.protected_read = protected_read;
    request.policy.read_exceptions = read_exceptions;
    request.environment = command.env_set;
    request.environment.emplace_back("PATH", default_search_path());
    request.environment.emplace_back("LC_ALL", "C.UTF-8");
    for (const auto& argument : command.argv) {
        if (!request.command.empty()) request.command += ' ';
        request.command += shell_quote(argument);
    }
    return run_mxc(request, options, output, stop);
}

} // namespace dagent::exec
