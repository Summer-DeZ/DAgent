#include <boost/test/unit_test.hpp>

#include <csignal>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tui/terminal.hpp"

using dagent::tui::Size;
using dagent::tui::Terminal;

// Terminal 会改写进程级状态（termios、信号处理器、stdout），
// 每个用例都在子进程里运行：子进程以退出码报告自身断言，
// 父进程读取它写出的全部字节。

namespace {

std::string read_all(int fd) {
    std::string out;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;  // EOF，或 pty 从端关闭后的 EIO
        }
    }
    return out;
}

int wait_exit_code(pid_t pid) {
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool same_termios(const termios& a, const termios& b) {
    return a.c_iflag == b.c_iflag && a.c_oflag == b.c_oflag &&
           a.c_cflag == b.c_cflag && a.c_lflag == b.c_lflag;
}

// 按顺序在 out 中依次查找各片段，返回全部找到且有序。
bool in_order(const std::string& out, std::initializer_list<std::string_view> parts) {
    std::size_t pos = 0;
    for (std::string_view p : parts) {
        pos = out.find(p, pos);
        if (pos == std::string::npos) return false;
        pos += p.size();
    }
    return true;
}

std::size_t count(const std::string& out, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t pos = out.find(needle); pos != std::string::npos;
         pos = out.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

void previous_handler(int) {}

} // namespace

BOOST_AUTO_TEST_SUITE(terminal)

BOOST_AUTO_TEST_CASE(tty_session_enters_and_restores_in_reverse_order) {
    int master = -1;
    winsize ws{};
    ws.ws_col = 100;
    ws.ws_row = 30;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 1);
        termios before{};
        ::tcgetattr(STDIN_FILENO, &before);
        {
            Terminal t;
            if (!(t.size() == Size{100, 30})) ::_exit(10);
            if (!t.caps().truecolor || !t.caps().synchronized) ::_exit(11);
            termios during{};
            ::tcgetattr(STDIN_FILENO, &during);
            if (during.c_lflag & (ICANON | ECHO)) ::_exit(12);
            t.set_mouse(true);
            t.set_mouse(true);  // 重复开启不重复发序列
            t.set_focus_events(true);
        }
        termios after{};
        ::tcgetattr(STDIN_FILENO, &after);
        ::_exit(same_termios(before, after) ? 0 : 13);
    }
    const std::string out = read_all(master);
    ::close(master);
    BOOST_TEST(wait_exit_code(pid) == 0);

    BOOST_TEST(in_order(out, {"\x1b[?1049h", "\x1b[?7l", "\x1b[?25l", "\x1b[?2004h",
                              "\x1b[?1000h\x1b[?1006h", "\x1b[?1004h",
                              "\x1b[?1006l\x1b[?1000l", "\x1b[?1004l", "\x1b[?2004l",
                              "\x1b[?25h", "\x1b[?7h", "\x1b[?1049l"}));
    BOOST_TEST(count(out, "\x1b[?1000h") == 1u);
    BOOST_TEST(count(out, "\x1b[?1049l") == 1u);  // 析构与 atexit 叠加也只还原一次
}

BOOST_AUTO_TEST_CASE(dumb_terminal_gets_no_optional_modes) {
    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, nullptr);
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::setenv("TERM", "dumb", 1);
        ::unsetenv("COLORTERM");
        {
            Terminal t;
            const auto& c = t.caps();
            if (c.truecolor || c.synchronized || c.sgr_mouse || c.bracketed_paste ||
                c.focus_events) {
                ::_exit(10);
            }
            t.set_mouse(true);
            t.set_focus_events(true);
        }
        ::_exit(0);
    }
    const std::string out = read_all(master);
    ::close(master);
    BOOST_TEST(wait_exit_code(pid) == 0);
    BOOST_TEST(in_order(out, {"\x1b[?1049h", "\x1b[?1049l"}));
    BOOST_TEST(out.find("\x1b[?2004") == std::string::npos);
    BOOST_TEST(out.find("\x1b[?1000") == std::string::npos);
    BOOST_TEST(out.find("\x1b[?1004") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(piped_stdout_receives_no_escape_bytes) {
    int fds[2];
    BOOST_REQUIRE(::pipe(fds) == 0);
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        const int null = ::open("/dev/null", O_RDONLY);
        ::dup2(null, STDIN_FILENO);
        ::setenv("TERM", "xterm-256color", 1);
        {
            Terminal t;
            if (!(t.size() == Size{80, 24})) ::_exit(10);  // 非 tty 回退尺寸
            t.set_mouse(true);
            t.set_focus_events(true);
            t.restore();
        }
        ::_exit(0);
    }
    ::close(fds[1]);
    const std::string out = read_all(fds[0]);
    ::close(fds[0]);
    BOOST_TEST(wait_exit_code(pid) == 0);
    BOOST_TEST(out.empty());
}

BOOST_AUTO_TEST_CASE(signal_wakes_self_pipe_and_previous_handler_is_restored) {
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        const int null = ::open("/dev/null", O_RDWR);
        ::dup2(null, STDIN_FILENO);
        ::dup2(null, STDOUT_FILENO);
        struct sigaction mine{};
        mine.sa_handler = &previous_handler;
        ::sigemptyset(&mine.sa_mask);
        ::sigaction(SIGTERM, &mine, nullptr);
        {
            Terminal t;
            pollfd p{t.signal_fd(), POLLIN, 0};
            if (::poll(&p, 1, 0) != 0) ::_exit(10);  // 无信号时不可读
            ::raise(SIGTERM);                          // 处理器只写管道，进程不退出
            if (::poll(&p, 1, 1000) != 1) ::_exit(11);
            const Terminal::Signals quit = t.drain_signal();
            if (!quit.quit || quit.resize) ::_exit(14); // 退出类信号如实分类
            if (::poll(&p, 1, 0) != 0) ::_exit(12);  // 读尽后不再可读

            // SIGWINCH：同样只唤醒，分类为尺寸变化而不是退出。
            ::raise(SIGWINCH);
            if (::poll(&p, 1, 1000) != 1) ::_exit(15);
            const Terminal::Signals winch = t.drain_signal();
            if (winch.quit || !winch.resize) ::_exit(16);

            // 两类信号挤在同一次清空里：两个标志都要报告。
            ::raise(SIGWINCH);
            ::raise(SIGHUP);
            const Terminal::Signals both = t.drain_signal();
            if (!both.quit || !both.resize) ::_exit(17);
        }
        struct sigaction now{};
        ::sigaction(SIGTERM, nullptr, &now);
        ::_exit(now.sa_handler == &previous_handler ? 0 : 13);
    }
    BOOST_TEST(wait_exit_code(pid) == 0);
}

BOOST_AUTO_TEST_SUITE_END()
