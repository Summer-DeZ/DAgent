#include "exec/process.hpp"

#include "base/log.hpp"
#include "base/text.hpp"
#include "exec/detail.hpp"
#include "exec/platform.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/asio/write.hpp>
#include <boost/process/v2.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dagent::exec {
namespace {

namespace asio = boost::asio;
namespace bp = boost::process::v2;


// 流式收集输出：head 保留前一半预算，tail 环形保留后一半，total 记录原始总量。
// Result 里按 base::truncate_middle 的语义合成，on_output 回调不受影响。
class OutputCollector {
public:
    explicit OutputCollector(std::size_t max_bytes)
        : head_cap_(max_bytes / 2), tail_cap_(max_bytes - max_bytes / 2) {}

    void append(std::string_view chunk) {
        total_ += chunk.size();
        if (head_.size() < head_cap_) {
            const std::size_t take = std::min(head_cap_ - head_.size(), chunk.size());
            head_.append(chunk.substr(0, take));
            chunk.remove_prefix(take);
        }
        if (chunk.empty()) return;
        tail_.append(chunk);
        if (tail_.size() > tail_cap_) tail_.erase(0, tail_.size() - tail_cap_);
    }

    std::string finish() const {
        if (total_ <= head_cap_ + tail_cap_) return head_ + tail_;

        const std::size_t head_end = base::utf8_floor(head_, head_.size());
        std::size_t tail_skip = 0;
        while (tail_skip < tail_.size() && (static_cast<unsigned char>(tail_[tail_skip]) & 0xC0) == 0x80)
            ++tail_skip;
        const std::size_t omitted = total_ - head_end - (tail_.size() - tail_skip);

        std::string text = head_.substr(0, head_end);
        text += "\n... " + std::to_string(omitted) + " bytes omitted ...\n";
        text += tail_.substr(tail_skip);
        return text;
    }

private:
    std::size_t head_cap_;
    std::size_t tail_cap_;
    std::string head_;
    std::string tail_;
    std::size_t total_ = 0;
};

class Runner {
public:
    Runner(const Command& cmd, const Options& opt,
           const std::function<void(Stream, std::string_view)>& on_output, std::stop_token stop)
        : cmd_(cmd),
          opt_(opt),
          on_output_(on_output),
          stop_(stop),
          in_pipe_(ctx_),
          out_pipe_(ctx_),
          err_pipe_(ctx_),
          timeout_timer_(ctx_),
          grace_timer_(ctx_),
          drain_timer_(ctx_),
          proc_(ctx_.get_executor()),
          out_(opt.max_output_bytes),
          err_(opt.max_output_bytes) {}

    // 所有退出路径（正常、超时、取消、回调异常）都保证清理整个进程组。
    ~Runner() {
        if (spawned_) group_.force_kill();
    }

    Result execute() {
        const auto start = std::chrono::steady_clock::now();
        spawn();

        // merge 模式下 stderr 走 stdout 管道，不存在独立的 err 流。
        if (cmd_.merge_stderr) err_eof_ = true;
        start_read_out();
        if (!cmd_.merge_stderr) start_read_err();
        start_stdin();

        proc_.async_wait([this](boost::system::error_code ec, int) {
            if (ec) {
                wait_error_ = ec;
                ctx_.stop();
                return;
            }
            process_exited_ = true;
            exit_status_ = detail::process_exit_status(proc_);
            if (!out_eof_ || !err_eof_) {
                drain_timer_.expires_after(opt_.drain_after_exit);
                drain_timer_.async_wait([this](boost::system::error_code timer_ec) {
                    if (!timer_ec) on_drain_expired();
                });
            }
            check_done();
        });

        const auto timeout = cmd_.timeout.value_or(opt_.default_timeout);
        if (timeout.count() > 0) {
            timeout_timer_.expires_after(timeout);
            timeout_timer_.async_wait([this](boost::system::error_code ec) {
                if (ec) return;
                timed_out_ = true;
                begin_terminate();
            });
        }

        std::stop_callback stop_cb(stop_, [this] {
            asio::post(ctx_, [this] {
                cancelled_ = true;
                begin_terminate();
            });
        });

        ctx_.run();

        if (user_error_ != nullptr) std::rethrow_exception(user_error_);
        if (cancelled_) throw ExecError{ExecError::Kind::cancelled, "command cancelled"};
        if (wait_error_) {
            base::logger("exec")->warn("wait for child failed: {}", wait_error_.message());
        }
        return build_result(start);
    }

private:
    void spawn() {
        if (cmd_.argv.empty() || cmd_.argv.front().empty())
            throw ExecError{ExecError::Kind::spawn_failed, "empty command"};

        auto env = detail::make_environment(cmd_, opt_);
        const auto program = detail::resolve_program(cmd_.argv.front(), cmd_.cwd, detail::find_env(env, "PATH"));
        if (program.empty())
            throw ExecError{ExecError::Kind::spawn_failed, "command not found: " + cmd_.argv.front()};
        detail::spawn_command(cmd_, program, env, ctx_, proc_, in_pipe_, out_pipe_, err_pipe_,
                              group_, spawned_);
    }

    void start_read_out() {
        out_pipe_.async_read_some(asio::buffer(out_buffer_), [this](boost::system::error_code ec, std::size_t bytes) {
            if (out_eof_) return;
            if (ec) {
                out_eof_ = true;
                check_done();
                return;
            }
            if (!consume(Stream::out, bytes)) return;
            start_read_out();
        });
    }

    void start_read_err() {
        err_pipe_.async_read_some(asio::buffer(err_buffer_), [this](boost::system::error_code ec, std::size_t bytes) {
            if (err_eof_) return;
            if (ec) {
                err_eof_ = true;
                check_done();
                return;
            }
            if (!consume(Stream::err, bytes)) return;
            start_read_err();
        });
    }

    bool consume(Stream stream, std::size_t bytes) {
        const std::string_view data((stream == Stream::out ? out_buffer_ : err_buffer_).data(), bytes);
        (stream == Stream::out ? out_ : err_).append(data);
        if (!on_output_) return true;
        try {
            on_output_(stream, data);
            return true;
        } catch (...) {
            user_error_ = std::current_exception();
            begin_terminate();
            return false;
        }
    }

    void start_stdin() {
        if (!cmd_.stdin_data) return;
        asio::async_write(in_pipe_, asio::buffer(*cmd_.stdin_data), [this](boost::system::error_code ec, std::size_t) {
            in_pipe_.close();
            if (ec && ec != asio::error::broken_pipe && ec != asio::error::operation_aborted)
                base::logger("exec")->debug("write child stdin: {}", ec.message());
        });
    }

    // 先对进程组发 SIGTERM，kill_grace 后 SIGKILL，只杀主进程会留下孤儿子孙。
    void begin_terminate() {
        if (terminating_) return;
        terminating_ = true;
        if (spawned_) group_.terminate();
        if (opt_.kill_grace.count() > 0) {
            grace_timer_.expires_after(opt_.kill_grace);
            grace_timer_.async_wait([this](boost::system::error_code ec) {
                if (!ec && spawned_) group_.force_kill();
            });
        } else if (spawned_) {
            group_.force_kill();
        }
    }

    // 主进程退出后仍然没有 EOF：后台进程还持有管道写端。
    // 杀掉整组、停止读取，不再等待。
    void on_drain_expired() {
        if (spawned_) group_.force_kill();
        stop_reads();
        check_done();
    }

    void stop_reads() {
        if (!out_eof_) {
            out_eof_ = true;
            out_pipe_.cancel();
            out_pipe_.close();
        }
        if (!err_eof_) {
            err_eof_ = true;
            err_pipe_.cancel();
            err_pipe_.close();
        }
        if (in_pipe_.is_open()) in_pipe_.close();
    }

    void check_done() {
        if (process_exited_ && out_eof_ && err_eof_) ctx_.stop();
    }

    Result build_result(std::chrono::steady_clock::time_point start) const {
        Result result;
        if (process_exited_) {
            result.exit_code = exit_status_.code;
            result.signal = exit_status_.signal;
        }
        result.timed_out = timed_out_;
        result.out = out_.finish();
        result.err = err_.finish();
        result.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        return result;
    }

    const Command& cmd_;
    const Options& opt_;
    std::function<void(Stream, std::string_view)> on_output_;
    std::stop_token stop_;

    asio::io_context ctx_;
    asio::writable_pipe in_pipe_;
    asio::readable_pipe out_pipe_;
    asio::readable_pipe err_pipe_;
    asio::steady_timer timeout_timer_;
    asio::steady_timer grace_timer_;
    asio::steady_timer drain_timer_;
    bp::process proc_;
    OutputCollector out_;
    OutputCollector err_;

    // 两路读取同时挂起：Asio 先把数据读进缓冲区、稍后才调回调，共用一块会互相覆盖。
    std::array<char, 64 << 10> out_buffer_{};
    std::array<char, 64 << 10> err_buffer_{};
    detail::ProcessGroup group_;
    detail::ExitStatus exit_status_;
    bool spawned_ = false;
    bool process_exited_ = false;
    bool out_eof_ = false;
    bool err_eof_ = false;
    bool terminating_ = false;
    bool timed_out_ = false;
    bool cancelled_ = false;
    std::exception_ptr user_error_;
    boost::system::error_code wait_error_;
};

} // namespace

Result run(const Command& cmd, const Options& opt,
           const std::function<void(Stream, std::string_view)>& on_output, std::stop_token stop) {
    if (stop.stop_requested()) throw ExecError{ExecError::Kind::cancelled, "command cancelled"};
    detail::ignore_sigpipe();
    Runner runner(cmd, opt, on_output, stop);
    return runner.execute();
}

std::optional<std::filesystem::path> which(std::string_view name) {
    const char* path_env = ::getenv("PATH");
    const auto path = detail::resolve_program(name, {}, path_env != nullptr ? path_env : "");
    if (path.empty()) return std::nullopt;
    return path;
}

std::string shell_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '\'';
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
}

} // namespace dagent::exec
