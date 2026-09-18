// L7 运行时验收：§3.9 定时器与 §3.8 选择复制的全链路。真实线程 + 真实
// 管道 / pty，不做模拟。
//   * 管道用例：子进程 stdin 接管道、stdout/stderr → /dev/null（Terminal 在
//     非 tty 下自动降级，不握手），驱动线程观察 Runtime 的诊断计数；
//   * pty 用例：子进程跑在 forkpty 的从端上，父进程在主端扮演终端 ——
//     注入 SGR 鼠标序列、读取 OSC 52 剪贴板序列。
// 子进程用退出码报告断言结果；alarm 兜底，死锁/空转的用例判失败。

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tui/app.hpp"
#include "tui/document.hpp"
#include "tui/layout.hpp"
#include "tui/widget.hpp"

using namespace dagent::tui;
using namespace std::chrono_literals;

namespace {

struct Child {
    pid_t pid = -1;
    int fd = -1; // 管道用例：子进程的键盘输入；pty 用例：主端
};

constexpr unsigned k_alarm_s = 10;

// 子进程：stdin 接管道读端、stdout/stderr → /dev/null。body 不返回。
template <class Body>
Child spawn_child(const Body& body) {
    int fds[2];
    BOOST_REQUIRE(::pipe(fds) == 0);
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        ::dup2(fds[0], STDIN_FILENO);
        ::close(fds[0]);
        const int null_fd = ::open("/dev/null", O_WRONLY);
        ::dup2(null_fd, STDOUT_FILENO);
        ::dup2(null_fd, STDERR_FILENO);
        ::alarm(k_alarm_s);
        body();
        ::_exit(99);
    }
    ::close(fds[0]);
    return {pid, fds[1]};
}

// 子进程跑在 cols×rows 的 pty 从端上。body 不返回。
template <class Body>
Child spawn_pty_child(const Body& body, int cols, int rows) {
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::alarm(k_alarm_s);
        body();
        ::_exit(99);
    }
    return {pid, master};
}

int exit_code(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int wait_exit_code(pid_t pid) {
    int status = 0;
    ::waitpid(pid, &status, 0);
    return exit_code(status);
}

// 持续读 pty 主端直到子进程退出（不读的话子进程写满缓冲会阻塞）。
int drain_pty_until_exit(Child c, std::string& out) {
    int status = 0;
    for (;;) {
        pollfd p{c.fd, POLLIN, 0};
        ::poll(&p, 1, 20);
        if ((p.revents & POLLIN) != 0) {
            char buf[4096];
            const ssize_t n = ::read(c.fd, buf, sizeof buf);
            if (n > 0) out.append(buf, static_cast<std::size_t>(n));
        }
        if (::waitpid(c.pid, &status, WNOHANG) == c.pid) break;
    }
    ::close(c.fd);
    return exit_code(status);
}

// 持续读 pty 主端，直到 out 中 from 之后出现 needle；返回 needle 的位置。
std::size_t read_until(Child& c, std::string& out, std::string_view needle,
                       std::size_t from = 0,
                       std::chrono::milliseconds limit = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
        const std::size_t at = out.find(needle, from);
        if (at != std::string::npos) return at;
        if (std::chrono::steady_clock::now() >= deadline) return std::string::npos;
        pollfd p{c.fd, POLLIN, 0};
        ::poll(&p, 1, 20);
        if ((p.revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
        char buf[4096];
        const ssize_t n = ::read(c.fd, buf, sizeof buf);
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
            return std::string::npos;
        }
    }
}

template <class Pred>
bool wait_until(const Pred& pred, std::chrono::milliseconds limit = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

std::string base64_decode(std::string_view s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int acc = 0;
    int bits = 0;
    for (const char c : s) {
        const int v = val(c);
        if (v < 0) break; // '=' 填充
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return out;
}

// 读到下一个 OSC 52 序列，返回 base64 解码后的负载；pos 推进到其后。
std::string next_clipboard(Child& c, std::string& out, std::size_t& pos) {
    const std::string_view head = "\x1b]52;c;";
    const std::size_t at = read_until(c, out, head, pos);
    if (at == std::string::npos) return "<no osc52>";
    const std::size_t end = read_until(c, out, "\x07", at);
    if (end == std::string::npos) return "<unterminated>";
    pos = end + 1;
    return base64_decode(std::string_view(out).substr(at + head.size(),
                                                      end - at - head.size()));
}

// 回车退出（全局处理器）。
struct QuitOnEnter : EventHandler {
    Runtime* rt = nullptr;
    bool on_event(const Event& e) override {
        if (e.kind == Event::Kind::key && e.key == Key::enter) {
            rt->quit();
            return true;
        }
        return false;
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(app)

BOOST_AUTO_TEST_CASE(timers_fire_in_due_order_and_respect_cancel) {
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        Terminal term;
        Runtime rt{term, root};

        std::string order;
        int ticks = 0;
        int self_cancel_runs = 0;
        // 建立顺序与到期顺序相反：按到期时刻执行；同时到期的按建立顺序。
        rt.after(60ms, [&] { order += 'c'; });
        rt.after(20ms, [&] { order += 'a'; });
        rt.after(40ms, [&] { order += 'b'; });
        rt.after(40ms, [&] { order += 'B'; });
        const TimerId dropped = rt.after(30ms, [&] { order += 'X'; });
        rt.cancel(dropped);
        rt.cancel(dropped); // 重复取消是空操作
        // every 返回 false 即停止；回调里取消自己同样停止。
        rt.every(5ms, [&] { return ++ticks < 3; });
        TimerId self = 0;
        self = rt.every(5ms, [&] {
            ++self_cancel_runs;
            rt.cancel(self);
            return true;
        });
        // 回调里新建定时器：留到下一轮执行。
        rt.after(70ms, [&] { rt.after(0ms, [&] { order += 'd'; }); });
        rt.after(150ms, [&] { rt.quit(); });
        rt.run();

        if (order != "abBcd") ::_exit(3);
        if (ticks != 3) ::_exit(4);
        if (self_cancel_runs != 1) ::_exit(5);
        ::_exit(0);
    });
    BOOST_TEST(wait_exit_code(c.pid) == 0);
    ::close(c.fd);
}

BOOST_AUTO_TEST_CASE(idle_runtime_without_timers_neither_wakes_nor_renders) {
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        root.add({Sizing::flex, 1}, std::make_unique<Text>());
        Terminal term;
        Runtime rt{term, root};

        // 一个短动画：跑 3 次后返回 false，之后没有任何定时器。
        std::atomic<int> ticks{0};
        rt.every(10ms, [&] { return ticks.fetch_add(1) + 1 < 3; });

        int code = 0;
        std::thread driver([&] {
            if (!wait_until([&] { return ticks.load() == 3 && rt.frames() >= 1; })) {
                code = 2;
            }
            std::this_thread::sleep_for(30ms); // 让最后一次唤醒落定
            const uint64_t w0 = rt.wakeups();
            const uint64_t f0 = rt.frames();
            std::this_thread::sleep_for(300ms);
            if (ticks.load() != 3) code = 3;       // every 停止后不再执行
            if (rt.wakeups() != w0) code = 4;      // 零唤醒：poll 无限期阻塞
            if (rt.frames() != f0) code = 5;       // 零帧
            rt.quit();
        });
        rt.run();
        driver.join();
        ::_exit(code);
    });
    BOOST_TEST(wait_exit_code(c.pid) == 0);
    ::close(c.fd);
}

BOOST_AUTO_TEST_CASE(drag_and_double_click_copy_source_text_via_osc52) {
    Child c = spawn_pty_child(
        [] {
            auto sb = std::make_unique<Scrollback>();
            Scrollback* sb_p = sb.get();
            sb_p->document().append_block(BlockKind::text, "hello world\nsecond line");
            LayerStack root{std::move(sb)};
            Terminal term;
            Runtime rt{term, root};
            ScrollbackMouse mouse{rt, *sb_p};
            rt.bind_mouse(*sb_p, mouse);
            QuitOnEnter quit;
            quit.rt = &rt;
            rt.set_global(quit);
            rt.run();
            ::_exit(0);
        },
        40, 10);

    std::string out;
    BOOST_REQUIRE(read_until(c, out, "\x1b[c") != std::string::npos); // 握手查询
    const std::string da1 = "\x1b[?62;22c";
    BOOST_REQUIRE(::write(c.fd, da1.data(), da1.size()) ==
                  static_cast<ssize_t>(da1.size()));
    BOOST_REQUIRE(read_until(c, out, "second") != std::string::npos); // 首帧已出
    std::size_t pos = out.size();

    // 从 (0,0) 拖到第二行列 5（"second" 的 d）并释放：跨硬换行的源文本。
    const std::string drag = "\x1b[<0;1;1M\x1b[<32;4;2M\x1b[<32;6;2M\x1b[<0;6;2m";
    BOOST_REQUIRE(::write(c.fd, drag.data(), drag.size()) ==
                  static_cast<ssize_t>(drag.size()));
    BOOST_TEST(next_clipboard(c, out, pos) == "hello world\nsecond");

    // 同一格快速双击 "world"：选词并复制。
    const std::string dbl = "\x1b[<0;9;1M\x1b[<0;9;1m\x1b[<0;9;1M\x1b[<0;9;1m";
    BOOST_REQUIRE(::write(c.fd, dbl.data(), dbl.size()) ==
                  static_cast<ssize_t>(dbl.size()));
    BOOST_TEST(next_clipboard(c, out, pos) == "world");

    BOOST_REQUIRE(::write(c.fd, "\r", 1) == 1);
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);
}

BOOST_AUTO_TEST_SUITE_END()
