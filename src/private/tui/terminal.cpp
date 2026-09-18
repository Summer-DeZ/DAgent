// pipe2 / cfmakeraw 属 GNU/BSD 扩展，-std=c++23（严格 ANSI）下 glibc 不予暴露。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "tui/terminal.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace dagent::tui {

namespace {

// 单实例约定下的全局锚点：atexit 兜底与信号处理器（无 this 可用）都需要它。
std::atomic<Terminal*> g_instance{nullptr};
std::atomic<int> g_signal_write_fd{-1};
std::once_flag g_atexit_once;

// 被接管的信号与其原有处理器：卸载时还原调用方自己装的处理器，
// 而不是粗暴地重置为 SIG_DFL。
// SIGWINCH 同样只写管道：尺寸仍由渲染线程 ioctl 读取，信号只负责
// 在空闲时唤醒（渲染按需驱动，没有信号就没有下一帧去探测尺寸）。
constexpr int kSignals[] = {SIGINT, SIGTERM, SIGHUP, SIGWINCH};

// 管道字节区分信号类别：'q' = 退出类，'w' = 尺寸变化。
constexpr char kQuitByte   = 'q';
constexpr char kResizeByte = 'w';
struct sigaction g_saved_handlers[std::size(kSignals)] = {};

// 异步信号处理器：只允许调用 async-signal-safe 的 write()。
// 惯例是 self-pipe —— 真正的还原与退出逻辑全部留在主循环的正常路径上做。
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

// 兜底路径 3：无论 main 如何返回（含遗漏 delete 的异常路径），退出前还原。
void terminal_atexit_restore() noexcept {
    if (Terminal* t = g_instance.load(std::memory_order_acquire)) {
        t->restore();
    }
}

bool env_has(std::string_view value, std::string_view key) noexcept {
    return value.find(key) != std::string_view::npos;
}

} // namespace

Terminal::Terminal() {
    probe_caps();

    // self-pipe 建立失败只意味着失去信号唤醒能力，不影响其余功能。
    if (::pipe2(signal_pipe_, O_CLOEXEC | O_NONBLOCK) == 0) {
        g_signal_write_fd.store(signal_pipe_[1], std::memory_order_release);
        install_signal_handlers();
    }

    // raw 模式：关行缓冲与回显，逐字节拿到输入。
    // stdin 非 tty（管道/重定向）时跳过，saved_ 保持无效。
    if (::isatty(STDIN_FILENO) && ::tcgetattr(STDIN_FILENO, &saved_) == 0) {
        raw_saved_ = true;
        termios raw = saved_;
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
    if (signal_pipe_[0] >= 0) {
        ::close(signal_pipe_[0]);
        ::close(signal_pipe_[1]);
        signal_pipe_[0] = -1;
        signal_pipe_[1] = -1;
    }
    if (g_instance.load(std::memory_order_acquire) == this) {
        g_instance.store(nullptr, std::memory_order_release);
    }
}

// 进入界面模式。stdout 非 tty 时不发任何序列（screen_active_ 保持
// false，restore/set_* 据此门控，避免把转义写进管道）。
// 顺序即文档约定的栈序：还原时严格逆序弹出。
void Terminal::enter() {
    if (!::isatty(STDOUT_FILENO)) {
        return;
    }

    std::string seq;
    seq.reserve(64);
    seq += "\x1b[?1049h"; // 备用屏幕：退出后完整还原用户原有内容
    seq += "\x1b[?7l";    // 关自动换行（DECAWM）：框架自己控制折行，
                          // 否则写满一行终端会自动折行，网格坐标全乱
    seq += "\x1b[?25l";   // 藏光标：帧末由渲染器定位后再显示
    if (caps_.bracketed_paste) {
        seq += "\x1b[?2004h"; // 粘贴内容被 \e[200~…\e[201~ 包裹，可区分逐字输入
        paste_.store(true, std::memory_order_release);
    }
    write(seq);
    screen_active_.store(true, std::memory_order_release);
}

// 能力探测：不查 terminfo，只用环境变量启发式 + 合理降级。
// modern = 现代终端的粗判；linux 控制台无 1006/dumb 无一切，全部按不支持处理。
void Terminal::probe_caps() noexcept {
    const char* term_env      = ::getenv("TERM");
    const char* colorterm_env = ::getenv("COLORTERM");
    std::string_view term      = term_env ? term_env : "";
    std::string_view colorterm = colorterm_env ? colorterm_env : "";

    const bool modern = !term.empty() && term != "dumb" && term != "linux";

    caps_.truecolor = env_has(colorterm, "truecolor") || env_has(colorterm, "24bit") ||
                      env_has(term, "truecolor") || env_has(term, "-direct");

    // DEC 2026 支持面：设置 COLORTERM 的（VTE 系）+ 已知支持的终端名。
    // 不支持的终端会忽略 2026 序列，误报无副作用。
    caps_.synchronized =
        modern && (!colorterm.empty() || env_has(term, "kitty") ||
                   env_has(term, "alacritty") || env_has(term, "foot") ||
                   env_has(term, "wezterm") || env_has(term, "contour") ||
                   env_has(term, "ghostty") || env_has(term, "iterm") ||
                   env_has(term, "tmux"));

    caps_.sgr_mouse       = modern;
    caps_.bracketed_paste = modern;
    caps_.focus_events    = modern;
}

Size Terminal::size() const noexcept {
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 &&
        ws.ws_row > 0) {
        return {static_cast<int>(ws.ws_col), static_cast<int>(ws.ws_row)};
    }
    return {80, 24};
}

// 阻塞式写满：渲染线程是唯一写者，不存在交错；pty 写出慢时被阻塞
// 属设计内行为（业务线程的更新在 post 队列里积压，不会被等锁）。
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

// exchange 去重：重复 set 不重复发序列。开与关的序列各自内部逆序配对。
// 未进入界面模式（stdout 非 tty）时是空操作，避免把转义写进管道。
void Terminal::set_mouse(bool on) {
    if (!screen_active_.load(std::memory_order_acquire) || !caps_.sgr_mouse ||
        mouse_.exchange(on, std::memory_order_acq_rel) == on) {
        return;
    }
    // 1002 = 按键事件跟踪：按下/释放 + 按住按键时的移动（拖拽选择必需，
    // §3.8）；1000 不报移动，1003 连悬停也报、事件量大且无用。
    write(on ? "\x1b[?1002h\x1b[?1006h" : "\x1b[?1006l\x1b[?1002l");
}

void Terminal::set_focus_events(bool on) {
    if (!screen_active_.load(std::memory_order_acquire) || !caps_.focus_events ||
        focus_.exchange(on, std::memory_order_acq_rel) == on) {
        return;
    }
    write(on ? "\x1b[?1004h" : "\x1b[?1004l");
}

// 握手结果以应答为准（§3.3）：Runtime 传入的副本已把「有应答」的项
// 覆盖为应答值、未应答的项保持初始值，这里直接落盘即可。
void Terminal::apply_caps(const Caps& caps) noexcept { caps_ = caps; }

bool Terminal::set_clipboard(std::string_view text) {
    if (!screen_active_.load(std::memory_order_acquire)) return false;
    constexpr size_t k_max = size_t{1} << 20;
    bool complete = true;
    if (text.size() > k_max) {
        size_t cut = k_max;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut; // 不从多字节字符中间截断
        }
        text = text.substr(0, cut);
        complete = false;
    }
    static constexpr char k_b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string seq = "\x1b]52;c;";
    seq.reserve(seq.size() + (text.size() + 2) / 3 * 4 + 1);
    size_t i = 0;
    for (; i + 3 <= text.size(); i += 3) {
        const uint32_t v = (static_cast<uint8_t>(text[i]) << 16) |
                           (static_cast<uint8_t>(text[i + 1]) << 8) |
                           static_cast<uint8_t>(text[i + 2]);
        seq += k_b64[(v >> 18) & 63];
        seq += k_b64[(v >> 12) & 63];
        seq += k_b64[(v >> 6) & 63];
        seq += k_b64[v & 63];
    }
    if (i < text.size()) {
        const bool two = i + 1 < text.size();
        const uint32_t v = (static_cast<uint8_t>(text[i]) << 16) |
                           (two ? static_cast<uint8_t>(text[i + 1]) << 8 : 0);
        seq += k_b64[(v >> 18) & 63];
        seq += k_b64[(v >> 12) & 63];
        seq += two ? k_b64[(v >> 6) & 63] : '=';
        seq += '=';
    }
    seq += '\a';
    write(seq);
    return complete;
}

void Terminal::set_kitty_keyboard(bool on) {
    if (!screen_active_.load(std::memory_order_acquire) ||
        !caps_.kitty_keyboard ||
        kitty_.exchange(on, std::memory_order_acq_rel) == on) {
        return;
    }
    write(on ? "\x1b[>1u" : "\x1b[<u");
}

// 还原 = 进入序列的严格逆序，只关自己开过的模式。
// restored_ 先行置位保证：析构 / atexit / 信号路径并发叠加时只执行一次。
// screen_active_ 门控：stdout 是管道时从未进入过界面模式，不得写出转义。
void Terminal::restore() noexcept {
    if (restored_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (screen_active_.load(std::memory_order_acquire)) {
        std::string seq;
        if (mouse_.exchange(false, std::memory_order_acq_rel)) {
            seq += "\x1b[?1006l\x1b[?1002l";
        }
        if (focus_.exchange(false, std::memory_order_acq_rel)) {
            seq += "\x1b[?1004l";
        }
        if (kitty_.exchange(false, std::memory_order_acq_rel)) {
            seq += "\x1b[<u"; // 弹出握手时推入的键盘 flag
        }
        if (paste_.exchange(false, std::memory_order_acq_rel)) {
            seq += "\x1b[?2004l";
        }
        seq += "\x1b[?25h";  // 显示光标
        seq += "\x1b[?7h";   // 恢复自动换行
        seq += "\x1b[?1049l"; // 离开备用屏，还原用户原有终端内容
        write(seq);
        screen_active_.store(false, std::memory_order_release);
    }

    if (raw_saved_) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
        raw_saved_ = false;
    }
}

// 管道 O_NONBLOCK：读尽即止（EAGAIN 使循环退出）。
Terminal::Signals Terminal::drain_signal() noexcept {
    Signals got;
    if (signal_pipe_[0] < 0) {
        return got;
    }
    char buf[64];
    ssize_t n;
    while ((n = ::read(signal_pipe_[0], buf, sizeof buf)) > 0) {
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
    // 保存原有处理器：卸载时还原调用方自己装的，而不是重置 SIG_DFL。
    for (std::size_t i = 0; i < std::size(kSignals); ++i) {
        ::sigaction(kSignals[i], &sa, &g_saved_handlers[i]);
    }
    handlers_installed_ = true;
}

void Terminal::uninstall_signal_handlers() noexcept {
    if (!handlers_installed_) {
        return;
    }
    // 先摘写端再撤处理器，避免竞态窗口内写已关闭的 fd。
    g_signal_write_fd.store(-1, std::memory_order_release);
    for (std::size_t i = 0; i < std::size(kSignals); ++i) {
        ::sigaction(kSignals[i], &g_saved_handlers[i], nullptr);
    }
    handlers_installed_ = false;
}

} // namespace dagent::tui
