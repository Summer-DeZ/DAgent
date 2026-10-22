// pipe2 / cfmakeraw 属 GNU/BSD 扩展，-std=c++23（严格 ANSI）下 glibc 不予暴露。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "tui/terminal.hpp"
#include "tui/event_wait.hpp"
#include "base/platform.hpp"

#include <cerrno>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <mutex>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace dagent::tui {

namespace {

// atexit 兜底与信号处理器（无 this 可用）所需的全局锚点。
std::atomic<Terminal*> g_instance{nullptr};

bool env_has(std::string_view value, std::string_view key) noexcept {
    return value.find(key) != std::string_view::npos;
}

std::atomic<int> g_signal_write_fd{-1};
std::once_flag g_atexit_once;

// 被接管的信号：卸载时还原调用方原装的处理器，而非 SIG_DFL。
constexpr int kSignals[] = {SIGINT, SIGTERM, SIGHUP, SIGWINCH};

// 管道字节区分信号类别。
constexpr char kQuitByte   = 'q'; ///< 退出类
constexpr char kResizeByte = 'w'; ///< 尺寸变化
struct sigaction g_saved_handlers[std::size(kSignals)] = {};

// 异步信号处理器：只 write self-pipe，逻辑留在主循环正常路径。
void terminal_on_signal(int sig) noexcept {
    const int saved_errno = errno; // 处理器不得改动被打断代码看到的 errno
    const int fd = g_signal_write_fd.load(std::memory_order_relaxed);
    if (fd >= 0) {
        const char b = sig == SIGWINCH ? kResizeByte : kQuitByte;
        ssize_t n = ::write(fd, &b, 1); // 管道满（EAGAIN）= 唤醒已挂起
        (void)n;
    }
    errno = saved_errno;
}

// atexit 兜底：进程退出前还原。
void terminal_atexit_restore() noexcept {
    if (Terminal* t = g_instance.load(std::memory_order_acquire)) {
        t->restore();
    }
}

} // namespace

struct Terminal::Native {
    termios saved_{};
    bool raw_saved_ = false;
    bool handlers_installed_ = false;
    int signal_pipe_[2] = {-1, -1};
};

// 能力探测：环境变量启发式，不查 terminfo。
void Terminal::probe_caps() noexcept {
    const char* term_env      = ::getenv("TERM");
    const char* colorterm_env = ::getenv("COLORTERM");
    std::string_view term      = term_env ? term_env : "";
    std::string_view colorterm = colorterm_env ? colorterm_env : "";

    const bool modern = !term.empty() && term != "dumb" && term != "linux";

    caps_.truecolor = env_has(colorterm, "truecolor") || env_has(colorterm, "24bit") ||
                      env_has(term, "truecolor") || env_has(term, "-direct");

    // DEC 2026：设置 COLORTERM 的终端 + 已知支持的终端名。
    caps_.synchronized =
        modern && (!colorterm.empty() || env_has(term, "kitty") ||
                   env_has(term, "alacritty") || env_has(term, "foot") ||
                   env_has(term, "wezterm") || env_has(term, "contour") ||
                   env_has(term, "ghostty") || env_has(term, "iterm") ||
                   env_has(term, "tmux"));

    caps_.sgr_mouse       = modern;
    caps_.bracketed_paste = modern;
}

Terminal::Terminal() {
    probe_caps();
    native_ = std::make_unique<Native>();

    // self-pipe 失败只失去信号唤醒能力，不影响其余功能。
    if (::pipe2(native_->signal_pipe_, O_CLOEXEC | O_NONBLOCK) == 0) {
        g_signal_write_fd.store(native_->signal_pipe_[1], std::memory_order_release);
        install_signal_handlers();
    }

    // raw 模式：关行缓冲与回显，逐字节拿到输入；stdin 非 tty 时跳过。
    if (base::stdin_is_terminal() && ::tcgetattr(STDIN_FILENO, &native_->saved_) == 0) {
        native_->raw_saved_ = true;
        termios raw = native_->saved_;
        ::cfmakeraw(&raw);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    enter();

    g_instance.store(this, std::memory_order_release);
    std::call_once(g_atexit_once, [] { std::atexit(&terminal_atexit_restore); });
}

Terminal::~Terminal() {
    restore();
    uninstall_signal_handlers();
    if (native_->signal_pipe_[0] >= 0) {
        ::close(native_->signal_pipe_[0]);
        ::close(native_->signal_pipe_[1]);
        native_->signal_pipe_[0] = -1;
        native_->signal_pipe_[1] = -1;
    }
    if (g_instance.load(std::memory_order_acquire) == this) {
        g_instance.store(nullptr, std::memory_order_release);
    }
}

bool Terminal::can_query() const noexcept {
    return native_->raw_saved_ && screen_active_.load(std::memory_order_acquire);
}

Size Terminal::size() const noexcept {
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 &&
        ws.ws_row > 0) {
        return {static_cast<int>(ws.ws_col), static_cast<int>(ws.ws_row)};
    }
    return {80, 24};
}

// 阻塞式写满。
void Terminal::write(std::string_view bytes) noexcept {
    while (!bytes.empty()) {
        ssize_t n = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        bytes.remove_prefix(static_cast<size_t>(n));
    }
}

void Terminal::restore_input() noexcept {
    if (native_->raw_saved_) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &native_->saved_);
        native_->raw_saved_ = false;
    }
}

// 读尽 self-pipe 即止（O_NONBLOCK，EAGAIN 退出循环）。
Terminal::Signals Terminal::drain_signal() noexcept {
    Signals got;
    if (native_->signal_pipe_[0] < 0) {
        return got;
    }
    char buf[64];
    ssize_t n;
    while ((n = ::read(native_->signal_pipe_[0], buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == kResizeByte) {
                got.resize = true;
            } else {
                got.quit = true;
            }
        }
    }
    return got;
}

void Terminal::install_signal_handlers() noexcept {
    struct sigaction sa {};
    sa.sa_handler = &terminal_on_signal;
    sa.sa_flags   = SA_RESTART; // 渲染线程的 write 不被信号打断
    ::sigemptyset(&sa.sa_mask);
    // 保存原有处理器供卸载时还原。
    for (std::size_t i = 0; i < std::size(kSignals); ++i) {
        ::sigaction(kSignals[i], &sa, &g_saved_handlers[i]);
    }
    native_->handlers_installed_ = true;
}

void Terminal::uninstall_signal_handlers() noexcept {
    if (!native_->handlers_installed_) {
        return;
    }
    // 先摘写端再撤处理器，避免竞态窗口内写已关闭的 fd。
    g_signal_write_fd.store(-1, std::memory_order_release);
    for (std::size_t i = 0; i < std::size(kSignals); ++i) {
        ::sigaction(kSignals[i], &g_saved_handlers[i], nullptr);
    }
    native_->handlers_installed_ = false;
}

struct EventWait::Impl {
    Terminal& terminal;
    int wake_pipe_[2] = {-1, -1};

    explicit Impl(Terminal& term) : terminal(term) {}
};

EventWait::EventWait(Terminal& terminal) : impl_(std::make_unique<Impl>(terminal)) {
    int pipes[2];
    if (::pipe2(pipes, O_NONBLOCK | O_CLOEXEC) != 0) {
        // 唤醒管道：创建失败直接终止。
        std::terminate();
    }
    impl_->wake_pipe_[0] = pipes[0];
    impl_->wake_pipe_[1] = pipes[1];
}

EventWait::~EventWait() {
    if (impl_->wake_pipe_[0] >= 0) ::close(impl_->wake_pipe_[0]);
    if (impl_->wake_pipe_[1] >= 0) ::close(impl_->wake_pipe_[1]);
}

EventWait::Ready EventWait::wait(int timeout_ms) noexcept {
    pollfd fds[3] = {
        {STDIN_FILENO, POLLIN, 0},
        {impl_->terminal.native_->signal_pipe_[0], POLLIN, 0},
        {impl_->wake_pipe_[0], POLLIN, 0},
    };
    const int rc = ::poll(fds, 3, timeout_ms);
    if (rc < 0) {
        if (errno == EINTR) return {Ready::Status::interrupted, false, false, false};
        return {Ready::Status::failed, false, false, false};
    }
    // POLLNVAL：不消费会使 poll 立即返回空转。
    return {Ready::Status::completed,
            (fds[1].revents & POLLIN) != 0,
            (fds[2].revents & POLLIN) != 0,
            (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0};
}

EventWait::Input EventWait::read_input(std::span<char> bytes) noexcept {
    ssize_t nread;
    do {
        nread = ::read(STDIN_FILENO, bytes.data(), bytes.size());
    } while (nread < 0 && errno == EINTR);
    if (nread == 0 || (nread < 0 && errno != EAGAIN)) return {0, true};
    if (nread > 0) return {static_cast<std::size_t>(nread), false};
    return {0, false};
}

void EventWait::wake() noexcept {
    const char b = 0;
    ssize_t n;
    do {
        n = ::write(impl_->wake_pipe_[1], &b, 1);
    } while (n < 0 && errno == EINTR);
    // 管道满：唤醒已挂起。
}

void EventWait::drain_wake() noexcept {
    char buf[64];
    for (;;) {
        const ssize_t n = ::read(impl_->wake_pipe_[0], buf, sizeof buf);
        if (n > 0 || (n < 0 && errno == EINTR)) continue;
        break; // EAGAIN = 已读尽
    }
}

} // namespace dagent::tui
