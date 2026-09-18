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
#include <csignal>
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

BOOST_AUTO_TEST_CASE(keymap_parses_bindings_and_dispatches) {
    // 在子进程里建 Runtime 与 Keymap，避免测试进程自己进入界面模式；
    // 事件直接喂给 Keymap（解析与分派是纯状态机，不需要终端）。
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        Terminal term;
        Runtime rt{term, root};
        Keymap km{rt};

        int a = 0;
        int b = 0;
        int ch = 0;
        int d = 0;
        int first = 0;
        int second = 0;
        int disabled_runs = 0;
        km.add({"a", "A", "", [&] { ++a; }, {}});
        km.add({"b", "B", "", [&] { ++b; }, {}});
        km.add({"c", "C", "", [&] { ++ch; }, {}});
        km.add({"d", "D", "", [&] { ++d; }, {}});
        km.add({"conflict", "X", "", [&] { ++first; }, {}});
        km.add({"conflict2", "X", "", [&] { ++second; }, {}});
        km.add({"off", "O", "", [&] { ++disabled_runs; },
                [] { return false; }});
        int shifted = 0;
        km.add({"shifted", "S", "", [&] { ++shifted; }, {}});

        if (!km.bind("ctrl+shift+a", "a")) ::_exit(3);
        if (!km.bind("alt+enter", "b")) ::_exit(4);
        if (!km.bind("f5", "c")) ::_exit(5);
        km.set_leader("ctrl+x", 60ms);
        if (!km.bind("<leader> ctrl+c", "d")) ::_exit(6);
        if (!km.bind("escape", "conflict")) ::_exit(7);
        if (!km.bind("escape", "conflict2")) ::_exit(8); // 后绑定覆盖先绑定
        if (!km.bind("ctrl+q", "off")) ::_exit(9);
        if (!km.bind("shift+g", "shifted")) ::_exit(40);

        const auto key = [](Key k, Mods m) {
            Event e;
            e.kind = Event::Kind::key;
            e.key = k;
            e.mods = m;
            return e;
        };
        const auto chr = [](std::string t, Mods m) {
            Event e;
            e.kind = any(m) ? Event::Kind::key : Event::Kind::text;
            e.mods = m;
            e.text = std::move(t);
            return e;
        };

        // ctrl+shift+a：kitty 上报大写 alternate，按基键小写匹配。
        if (!km.on_event(chr("A", Mods::ctrl | Mods::shift))) ::_exit(10);
        if (a != 1) ::_exit(11);
        if (!km.on_event(key(Key::enter, Mods::alt))) ::_exit(12);
        if (b != 1) ::_exit(13);
        if (!km.on_event(key(Key::f5, Mods::none))) ::_exit(14);
        if (ch != 1) ::_exit(15);
        // <leader> ctrl+c
        if (!km.on_event(chr("x", Mods::ctrl))) ::_exit(16);
        if (!km.on_event(chr("c", Mods::ctrl))) ::_exit(17);
        if (d != 1) ::_exit(18);
        // escape 后绑定覆盖先绑定
        if (!km.on_event(key(Key::escape, Mods::none))) ::_exit(19);
        if (first != 0 || second != 1) ::_exit(20);
        // enabled 返回 false：按键被消费但不执行
        if (!km.on_event(chr("q", Mods::ctrl))) ::_exit(21);
        if (disabled_runs != 0) ::_exit(22);
        // Shift+字母：终端报成无修饰的文本 "G"，与 "shift+g" 匹配；小写 g
        // 不匹配；绑定串写 "G" 与 "shift+g" 是同一序列（后绑定覆盖）。
        if (!km.on_event(chr("G", Mods::none))) ::_exit(41);
        if (km.on_event(chr("g", Mods::none))) ::_exit(42);
        if (shifted != 1) ::_exit(43);
        if (!km.bind("G", "a")) ::_exit(44);
        if (!km.on_event(chr("G", Mods::none)) || shifted != 1 || a != 2) ::_exit(45);

        // 非法串：空、裸露修饰键、未知修饰键/键名、越界功能键。
        const char* bad[] = {"", "   ", "ctrl+", "+a", "hyper+a",
                             "nope", "f13", "ctrl+shift+"};
        for (const char* s : bad) {
            if (km.bind(s, "a")) ::_exit(30);
        }
        if (km.bind("ctrl+b", "missing")) ::_exit(31); // 命令不存在
        // 合法：单键、前后多余空格。
        if (!km.bind("ctrl+x", "a")) ::_exit(32);
        if (!km.bind("  ctrl+a   ", "b")) ::_exit(33);
        if (km.commands().size() != 8) ::_exit(34);
        ::_exit(0);
    });
    BOOST_TEST(wait_exit_code(c.pid) == 0);
    ::close(c.fd);
}

BOOST_AUTO_TEST_CASE(keymap_leader_executes_and_timeout_falls_through) {
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        auto box = std::make_unique<InputBox>();
        InputBox* const box_p = box.get();
        root.add({Sizing::flex, 1}, std::move(box));
        Terminal term;
        Runtime rt{term, root};
        InputBoxHandler input{*box_p};
        rt.set_focus(&input, box_p);
        Keymap km{rt};
        rt.set_global(km);

        int runs = 0;
        km.add({"test.n", "N", "", [&] { ++runs; }, {}});
        km.set_leader("ctrl+x", 80ms);
        if (!km.bind("<leader> n", "test.n")) ::_exit(3);
        rt.after(600ms, [&] { rt.quit(); });
        rt.run();

        if (runs != 1) ::_exit(4);        // leader 后的 n 执行命令
        if (box_p->text() != "n") ::_exit(5); // 超时后的 n 才进输入框
        ::_exit(0);
    });

    // ctrl+x 后按 n：Keymap 自压模态，n 不进输入框。
    BOOST_REQUIRE(::write(c.fd, "\x18", 1) == 1);
    std::this_thread::sleep_for(20ms);
    BOOST_REQUIRE(::write(c.fd, "n", 1) == 1);
    std::this_thread::sleep_for(120ms); // 上一序列状态落定

    // 再按一次 leader 后停手：等超过 leader 超时（80ms），模态已弹出，
    // 随后的 n 走焦点链进输入框；若超时没生效，它会完成 <leader> n
    // 让 runs 变成 2（子进程据此判失败）。
    BOOST_REQUIRE(::write(c.fd, "\x18", 1) == 1);
    std::this_thread::sleep_for(200ms);
    BOOST_REQUIRE(::write(c.fd, "n", 1) == 1);
    BOOST_TEST(wait_exit_code(c.pid) == 0);
    ::close(c.fd);
}

BOOST_AUTO_TEST_CASE(run_external_suspends_and_resumes_with_full_repaint) {
    Child c = spawn_pty_child(
        [] {
            Container root{Container::Direction::vertical};
            auto first = std::make_unique<Text>();
            first->set_text("alpha");
            root.add({Sizing::fixed, 1}, std::move(first));
            auto second = std::make_unique<Text>();
            second->set_text("bravo");
            root.add({Sizing::fixed, 1}, std::move(second));
            Terminal term;
            Runtime rt{term, root};
            rt.after(80ms, [&] {
                rt.run_external([] {
                    constexpr std::string_view line = "EXTERNAL-EDITOR\n";
                    if (::write(STDOUT_FILENO, line.data(), line.size()) < 0) {
                        ::_exit(8);
                    }
                    // 外部程序运行期间用户按 Ctrl+C：规范模式下 SIGINT 送达
                    // 整个前台进程组，本进程也会收到 —— 不能因此退出。
                    ::kill(::getpid(), SIGINT);
                });
            });
            bool survived = false;
            rt.after(300ms, [&] {
                survived = true;
                rt.quit();
            });
            rt.run();
            ::_exit(survived ? 0 : 7);
        },
        40, 10);

    std::string out;
    BOOST_REQUIRE(read_until(c, out, "alpha") != std::string::npos); // 首帧
    const std::size_t ext = read_until(c, out, "EXTERNAL-EDITOR");
    BOOST_REQUIRE(ext != std::string::npos);
    // 恢复后的第一帧整屏重画：两行在重新进入备用屏之后再次写出。
    BOOST_REQUIRE(read_until(c, out, "alpha", ext) != std::string::npos);
    BOOST_REQUIRE(read_until(c, out, "bravo", ext) != std::string::npos);

    const std::size_t leave_at = out.rfind("\x1b[?1049l", ext);
    const std::size_t enter_at = out.find("\x1b[?1049h", ext);
    const std::size_t alpha_at = out.find("alpha", ext);
    BOOST_TEST(leave_at != std::string::npos);   // 挂起离开备用屏
    BOOST_TEST(enter_at != std::string::npos);   // 恢复重新进入
    BOOST_TEST(leave_at < ext);                  // 外部文本在界面模式之外
    BOOST_TEST(ext < enter_at);
    BOOST_TEST(enter_at < alpha_at);             // 恢复后整行写出 alpha/bravo
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);
}

BOOST_AUTO_TEST_SUITE_END()
