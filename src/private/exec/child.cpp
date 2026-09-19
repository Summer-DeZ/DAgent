#include "exec/child.hpp"

#include "base/log.hpp"
#include "exec/detail.hpp"
#include "exec/sandbox.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/asio/write.hpp>
#include <boost/process/v2.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <deque>
#include <exception>
#include <istream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dagent::exec {
namespace {

namespace asio = boost::asio;
namespace bp = boost::process::v2;


/// 默认 launcher 的 env 成员默认指向进程环境；这个 init 把它换成过滤后的环境。
/// 不用 Boost.Process 的 process_environment：1.83 的 header-only 模式下它没有提供
/// on_setup 的定义（需要单独编译的 libboost_process）。
struct EnvSetup {
    std::vector<const char*> entries;

    explicit EnvSetup(const std::vector<std::string>& env) {
        entries.reserve(env.size() + 1);
        for (const auto& entry : env) entries.push_back(entry.c_str());
        entries.push_back(nullptr);  // execve 要求以 nullptr 结尾
    }

    boost::system::error_code on_setup(bp::posix::default_launcher& launcher, const bp::filesystem::path&,
                                       const char* const*) const {
        launcher.env = entries.data();
        return {};
    }
};

/// fork 之后、exec 之前执行的钩子，和 process.cpp 共用 detail::setup_child。
struct ExecSetup {
    const Prepared* sandbox = nullptr;
    const char* cwd = nullptr;

    boost::system::error_code on_exec_setup(bp::posix::default_launcher&, const bp::filesystem::path&,
                                          const char* const*) const {
        const int err = detail::setup_child(sandbox, cwd);
        if (err != 0) errno = err; // 失败时 launcher 把 errno（而不是返回的 ec）写回父进程
        return {err, boost::system::system_category()};
    }

    /// exec 前失败时抢先 _exit，不让 launcher 的 ::exit 重刷父进程的 stdio 缓冲（见 process.cpp）。
    template <class... Args>
    [[noreturn]] void on_exec_error(Args&&...) const noexcept {
        ::_exit(127);
    }
};

} // namespace

struct Child::Impl {
    using LineCallback = std::function<void(std::string_view)>;
    using ExitCallback = std::function<void(std::optional<int>, std::optional<int>)>;

    struct ExitResult {
        std::optional<int> code;
        std::optional<int> signal;
    };

    static constexpr std::size_t kMaxPendingBytes = 8 << 20;

    Impl(const Command& cmd, const Options& opt)
        : opt_(opt),
          ctx_(),
          in_pipe_(ctx_),
          out_pipe_(ctx_),
          err_pipe_(ctx_),
          grace_timer_(ctx_),
          drain_timer_(ctx_),
          proc_(ctx_.get_executor()) {
        start(cmd);
    }

    ~Impl() { terminate(); }

    void write(std::string_view data) {
        {
            std::lock_guard lock(mutex_);
            if (exited_) return;
            if (pending_bytes_ + data.size() > kMaxPendingBytes) {
                base::logger("exec")->error("child stdin backlog exceeds {} bytes, dropping write", kMaxPendingBytes);
                return;
            }
            pending_bytes_ += data.size();
            writes_.emplace_back(data);
        }
        asio::post(ctx_, [this] { start_write(); });
    }

    void set_on_line(LineCallback cb) {
        std::lock_guard lock(mutex_);
        on_line_ = std::move(cb);
    }

    void set_on_exit(ExitCallback cb) {
        ExitCallback deliver;
        ExitResult result;
        {
            std::lock_guard lock(mutex_);
            on_exit_ = std::move(cb);
            if (exit_happened_ && !exit_delivered_ && on_exit_) {
                exit_delivered_ = true;
                deliver = on_exit_;
                result = exit_result_;
            }
        }
        if (deliver) invoke_exit(deliver, result);
    }

    void terminate() {
        request_terminate();
        std::lock_guard lock(life_mutex_);
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    }

private:
    void start(const Command& cmd) {
        if (cmd.argv.empty() || cmd.argv.front().empty())
            throw ExecError{ExecError::Kind::spawn_failed, "empty command"};
        auto env = detail::make_environment(cmd, opt_);
        program_ = detail::resolve_program(cmd.argv.front(), cmd.cwd, detail::find_env(env, "PATH"));
        if (program_.empty())
            throw ExecError{ExecError::Kind::spawn_failed, "command not found: " + cmd.argv.front()};
        const EnvSetup env_setup(env);
        detail::ignore_sigpipe();  // MCP server 挂掉后再 write 应得到 EPIPE，而不是杀掉 agent

        detail::PipeFds in, out, err;
        detail::make_pipe(in);
        detail::make_pipe(out);
        detail::make_pipe(err);

        std::vector<std::string> args(cmd.argv.begin() + 1, cmd.argv.end());
        ExecSetup setup{cmd.sandbox, cmd.cwd.empty() ? nullptr : cmd.cwd.c_str()};
        bp::process_stdio stdio{
            .in = bp::detail::process_input_binding{in.read},
            .out = bp::detail::process_output_binding{out.write},
            .err = bp::detail::process_error_binding{err.write},
        };

        try {
            proc_ = bp::process(ctx_, program_, args, env_setup, setup, stdio);
        } catch (const boost::system::system_error& e) {
            throw ExecError{ExecError::Kind::spawn_failed, e.what()};
        }
        pid_ = proc_.id();

        ::close(in.read);
        in.read = -1;
        ::close(out.write);
        out.write = -1;
        ::close(err.write);
        err.write = -1;
        in_pipe_.assign(in.write);
        in.write = -1;
        out_pipe_.assign(out.read);
        out.read = -1;
        err_pipe_.assign(err.read);
        err.read = -1;

        read_stdout();
        read_stderr();
        proc_.async_wait([this](boost::system::error_code ec, int) { on_process_exit(ec); });
        thread_ = std::jthread([this] { ctx_.run(); });
    }

    void start_write() {
        {
            std::lock_guard lock(mutex_);
            if (writing_ || writes_.empty() || exited_) return;
            writing_ = true;
            write_buffer_ = std::move(writes_.front());
            writes_.pop_front();
        }
        asio::async_write(in_pipe_, asio::buffer(write_buffer_), [this](boost::system::error_code ec, std::size_t) {
            {
                std::lock_guard lock(mutex_);
                writing_ = false;
                pending_bytes_ -= write_buffer_.size();
            }
            if (ec) {
                if (ec != asio::error::operation_aborted && ec != asio::error::broken_pipe)
                    base::logger("exec")->debug("write child stdin: {}", ec.message());
                return;
            }
            start_write();
        });
    }

    static std::string take_line(asio::streambuf& buffer) {
        std::istream stream(&buffer);
        std::string line;
        std::getline(stream, line);
        return line;
    }

    void read_stdout() {
        asio::async_read_until(out_pipe_, out_buffer_, '\n', [this](boost::system::error_code ec, std::size_t) {
            if (out_closed_) return;
            if (ec == asio::error::not_found) {
                deliver_line(take_line(out_buffer_));
                read_stdout();
                return;
            }
            if (ec) {
                if (out_buffer_.size() > 0) deliver_line(take_line(out_buffer_));
                out_closed_ = true;
                check_done();
                return;
            }
            deliver_line(take_line(out_buffer_));
            read_stdout();
        });
    }

    void read_stderr() {
        asio::async_read_until(err_pipe_, err_buffer_, '\n', [this](boost::system::error_code ec, std::size_t) {
            if (err_closed_) return;
            if (ec == asio::error::not_found) {
                log_line(take_line(err_buffer_));
                read_stderr();
                return;
            }
            if (ec) {
                if (err_buffer_.size() > 0) log_line(take_line(err_buffer_));
                err_closed_ = true;
                check_done();
                return;
            }
            log_line(take_line(err_buffer_));
            read_stderr();
        });
    }

    void deliver_line(std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        LineCallback cb;
        {
            std::lock_guard lock(mutex_);
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
        if (!line.empty() && line.back() == '\r') line.pop_back();
        base::logger("mcp")->info("{}", line);
    }

    void on_process_exit(boost::system::error_code ec) {
        if (ec) {
            base::logger("exec")->warn("wait for child failed: {}", ec.message());
        } else {
            process_exited_ = true;
            raw_status_ = proc_.native_exit_code();
        }
        if (out_closed_ && err_closed_) {
            finish();
            return;
        }
        drain_timer_.expires_after(opt_.drain_after_exit);
        drain_timer_.async_wait([this](boost::system::error_code timer_ec) {
            if (timer_ec) return;
            if (pid_ > 0) ::kill(-pid_, SIGKILL);  // 后台进程还占着管道
            stop_reads();
            finish();
        });
    }

    void stop_reads() {
        if (!out_closed_) {
            out_closed_ = true;
            out_pipe_.cancel();
            out_pipe_.close();
        }
        if (!err_closed_) {
            err_closed_ = true;
            err_pipe_.cancel();
            err_pipe_.close();
        }
        if (in_pipe_.is_open()) in_pipe_.close();
    }

    void check_done() {
        if (process_exited_ && out_closed_ && err_closed_) finish();
    }

    void finish() {
        if (finished_) return;
        finished_ = true;
        stop_reads();

        ExitResult result;
        if (process_exited_) {  // 等待失败时状态未知，两者都留空
            if (WIFEXITED(raw_status_)) result.code = WEXITSTATUS(raw_status_);
            else if (WIFSIGNALED(raw_status_)) result.signal = WTERMSIG(raw_status_);
        }

        ExitCallback deliver;
        {
            std::lock_guard lock(mutex_);
            exited_ = true;
            exit_happened_ = true;
            exit_result_ = result;
            if (on_exit_ && !exit_delivered_) {
                exit_delivered_ = true;
                deliver = on_exit_;
            }
        }
        ctx_.stop();
        if (deliver) invoke_exit(deliver, result);
    }

    void invoke_exit(const ExitCallback& cb, const ExitResult& result) {
        try {
            cb(result.code, result.signal);
        } catch (const std::exception& e) {
            base::logger("exec")->error("child on_exit callback threw: {}", e.what());
        }
    }

    void request_terminate() {
        if (terminating_.exchange(true)) return;
        asio::post(ctx_, [this] {
            if (pid_ <= 0) return;
            ::kill(-pid_, SIGTERM);
            if (opt_.kill_grace.count() > 0) {
                grace_timer_.expires_after(opt_.kill_grace);
                grace_timer_.async_wait([this](boost::system::error_code ec) {
                    if (!ec && pid_ > 0) ::kill(-pid_, SIGKILL);
                });
            } else {
                ::kill(-pid_, SIGKILL);
            }
        });
    }

    Options opt_;
    asio::io_context ctx_;
    asio::writable_pipe in_pipe_;
    asio::readable_pipe out_pipe_;
    asio::readable_pipe err_pipe_;
    asio::steady_timer grace_timer_;
    asio::steady_timer drain_timer_;
    asio::streambuf out_buffer_;
    asio::streambuf err_buffer_;
    bp::process proc_;
    std::filesystem::path program_;

    std::mutex mutex_;
    std::mutex life_mutex_;
    LineCallback on_line_;
    ExitCallback on_exit_;
    std::deque<std::string> writes_;
    std::string write_buffer_;
    std::size_t pending_bytes_ = 0;
    bool writing_ = false;
    bool exited_ = false;
    bool exit_happened_ = false;
    bool exit_delivered_ = false;
    ExitResult exit_result_;

    int pid_ = -1;
    int raw_status_ = 0;
    bool process_exited_ = false;
    bool out_closed_ = false;
    bool err_closed_ = false;
    bool finished_ = false;
    std::atomic<bool> terminating_{false};
    std::jthread thread_;
};

Child::Child() = default;
Child::~Child() = default;

std::unique_ptr<Child> Child::spawn(const Command& cmd, const Options& opt) {
    auto child = std::unique_ptr<Child>(new Child());
    child->impl_ = std::make_unique<Impl>(cmd, opt);
    return child;
}

void Child::write(std::string_view data) { impl_->write(data); }

void Child::on_line(std::function<void(std::string_view)> cb) { impl_->set_on_line(std::move(cb)); }

void Child::on_exit(std::function<void(std::optional<int>, std::optional<int>)> cb) {
    impl_->set_on_exit(std::move(cb));
}

void Child::terminate() { impl_->terminate(); }

} // namespace dagent::exec
