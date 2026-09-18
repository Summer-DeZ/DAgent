// 终端与运行时（§4.1、§4.7、§4.8）：真实子进程 + 真实管道 / pty，不做模拟。
//   * 管道用例：子进程 stdin 接管道、stdout/stderr → /dev/null（Terminal 在非 tty
//     下不进入界面模式、不握手），父进程往管道里写按键字节；
//   * pty 用例：子进程跑在 forkpty 的从端上，父进程在主端扮演终端 —— 读取查询、
//     写回应答、注入按键与 SGR 鼠标序列、检查输出。
// 子进程用退出码报告断言结果；alarm 兜底，死锁/空转判失败。

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tui/runtime.hpp"
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

// 子进程跑在 40×10 的 pty 从端上。body 不返回。
template <class Body>
Child spawn_pty_child(const Body& body) {
    winsize ws{};
    ws.ws_col = 40;
    ws.ws_row = 10;
    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        // 固定终端环境：能力的初始猜测不随运行测试的终端变化。
        ::setenv("TERM", "xterm-256color", 1);
        ::unsetenv("COLORTERM");
        ::alarm(k_alarm_s);
        body();
        ::_exit(99);
    }
    return {pid, master};
}

int exit_code(int status) { return WIFEXITED(status) ? WEXITSTATUS(status) : -1; }

int wait_exit_code(Child c) {
    int status = 0;
    ::waitpid(c.pid, &status, 0);
    ::close(c.fd);
    return exit_code(status);
}

// 关闭键盘输入（子进程读到 EOF 即退出主循环）并等待退出。
int close_input_and_wait(Child c) {
    ::close(c.fd);
    int status = 0;
    ::waitpid(c.pid, &status, 0);
    return exit_code(status);
}

void send(Child c, std::string_view bytes) {
    BOOST_REQUIRE(::write(c.fd, bytes.data(), bytes.size()) ==
                  static_cast<ssize_t>(bytes.size()));
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
std::size_t read_until(Child c, std::string& out, std::string_view needle,
                       std::size_t from = 0) {
    const auto deadline = std::chrono::steady_clock::now() + 3000ms;
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

// 读到下一个 OSC 52 序列，返回解码后的剪贴板内容；pos 推进到其后。
std::string next_clipboard(Child c, std::string& out, std::size_t& pos) {
    const std::string_view head = "\x1b]52;c;";
    const std::size_t at = read_until(c, out, head, pos);
    if (at == std::string::npos) return "<no osc52>";
    const std::size_t end = read_until(c, out, "\x07", at);
    if (end == std::string::npos) return "<unterminated>";
    pos = end + 1;
    return base64_decode(std::string_view(out).substr(at + head.size(), end - at - head.size()));
}

// 用 lambda 写的事件处理器。
struct Handler : EventHandler {
    std::function<bool(const Event&)> fn;
    explicit Handler(std::function<bool(const Event&)> f) : fn(std::move(f)) {}
    bool on_event(const Event& e) override { return fn(e); }
};

bool is_key(const Event& e, Key k) { return e.kind == Event::Kind::key && e.key == k; }

std::unique_ptr<Text> text_widget(std::string s) {
    auto t = std::make_unique<Text>();
    t->set_text(std::move(s));
    return t;
}

} // namespace

BOOST_AUTO_TEST_SUITE(runtime)

// 界面模式的进入、暂离与还原：run_external 期间退出备用屏与鼠标上报、外部程序的
// Ctrl+C 不结束界面；SIGTERM 正常退出，终端模式与 termios 全部还原。
BOOST_AUTO_TEST_CASE(terminal_modes_suspend_resume_and_restore) {
    Child c = spawn_pty_child([] {
        termios before{};
        ::tcgetattr(STDIN_FILENO, &before);
        bool survived = false;
        {
            Container root{Container::Direction::vertical};
            root.add({Sizing::fixed, 1}, text_widget("alpha"));
            Terminal term;
            term.set_mouse(true);
            Runtime rt{term, root};
            rt.after(80ms, [&] {
                rt.run_external([] {
                    constexpr std::string_view line = "EXTERNAL-EDITOR\n";
                    if (::write(STDOUT_FILENO, line.data(), line.size()) < 0) ::_exit(8);
                    ::kill(::getpid(), SIGINT); // 用户在外部程序里按 Ctrl+C
                });
                rt.after(100ms, [&] { survived = true; });
            });
            rt.run(); // SIGTERM → 正常返回
        }
        termios after{};
        ::tcgetattr(STDIN_FILENO, &after);
        const tcflag_t mask = ICANON | ECHO | ISIG;
        if ((after.c_lflag & mask) != (before.c_lflag & mask)) ::_exit(3);
        ::_exit(survived ? 0 : 4);
    });

    std::string out;
    BOOST_REQUIRE(read_until(c, out, "\x1b[c") != std::string::npos);
    send(c, "\x1b[?62;22c");
    const std::size_t first = read_until(c, out, "alpha");
    BOOST_REQUIRE(first != std::string::npos);
    const std::size_t ext = read_until(c, out, "EXTERNAL-EDITOR", first);
    BOOST_REQUIRE(ext != std::string::npos);
    const std::size_t repaint = read_until(c, out, "alpha", ext); // 恢复后整屏重画
    BOOST_REQUIRE(repaint != std::string::npos);
    std::this_thread::sleep_for(250ms);
    BOOST_REQUIRE(::kill(c.pid, SIGTERM) == 0);
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);

    const std::string_view before_ext = std::string_view(out).substr(first, ext - first);
    const std::string_view after_ext = std::string_view(out).substr(ext, repaint - ext);
    BOOST_TEST(before_ext.find("\x1b[?1006l\x1b[?1002l") != std::string_view::npos);
    BOOST_TEST(before_ext.ends_with("\x1b[?1049l"));
    BOOST_TEST(after_ext.find("\x1b[?1049h") != std::string_view::npos);
    BOOST_TEST(after_ext.find("\x1b[?1002h\x1b[?1006h") != std::string_view::npos);
    const std::string_view tail = std::string_view(out).substr(repaint);
    BOOST_TEST(tail.find("\x1b[?1006l\x1b[?1002l") != std::string_view::npos);
    BOOST_TEST(tail.ends_with("\x1b[?25h\x1b[?7h\x1b[?1049l"));
}

// 能力握手不阻塞首帧；应答确定后开启 kitty 键盘与字素宽度模式，之后的帧包在同步
// 输出里；on_caps 恰好回调一次，据背景色选出亮色主题。
BOOST_AUTO_TEST_CASE(handshake_detects_caps_without_blocking_first_frame) {
    Child c = spawn_pty_child([] {
        auto label = text_widget("waiting");
        Text* const label_p = label.get();
        LayerStack root{std::move(label)};
        Terminal term;
        Runtime rt{term, root};
        Handler quit{[&](const Event& e) {
            if (is_key(e, Key::enter)) rt.quit();
            return true;
        }};
        rt.set_global(quit);
        int calls = 0;
        Terminal::Caps got{};
        const ThemeTokens* theme = nullptr;
        rt.on_caps([&](const Terminal::Caps& caps) {
            ++calls;
            got = caps;
            theme = &default_theme(caps.background);
            label_p->set_text("caps-ok");
        });
        rt.run();
        if (calls != 1) ::_exit(3);
        if (!got.synchronized || !got.grapheme_width || !got.kitty_keyboard) ::_exit(4);
        if (!(got.background == Color::rgb(255, 255, 255))) ::_exit(5);
        if (theme != &light_theme()) ::_exit(6);
        int late = 0;
        rt.on_caps([&](const Terminal::Caps&) { ++late; }); // 已确定：立即回调
        ::_exit(late == 1 ? 0 : 7);
    });

    std::string out;
    BOOST_REQUIRE(read_until(c, out, "\x1b[c") != std::string::npos);
    BOOST_REQUIRE(read_until(c, out, "waiting") != std::string::npos); // 首帧先出
    send(c, "\x1b[?2026;2$y"          // 同步输出：支持
            "\x1b[?2027;1$y"          // 字素宽度模式：支持
            "\x1b[?0u"                // kitty 键盘协议：有应答即支持
            "\x1b]11;rgb:ffff/ffff/ffff\x1b\\" // 白底
            "\x1b[?62;22c");          // DA1 哨兵
    const std::size_t done = read_until(c, out, "caps-ok");
    BOOST_REQUIRE(done != std::string::npos);
    const std::size_t kitty = out.find("\x1b[>1u");
    const std::size_t width_mode = out.find("\x1b[?2027h");
    const std::size_t sync = out.rfind("\x1b[?2026h", done);
    BOOST_TEST(kitty != std::string::npos);
    BOOST_TEST(width_mode != std::string::npos);
    BOOST_TEST(out.find("waiting") < out.find("\x1b[?2026h")); // 首帧未同步包帧
    BOOST_TEST(sync != std::string::npos);
    BOOST_TEST(kitty < sync);
    send(c, "\r");
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);
}

// 键盘输入：字节解码（多字节字符跨读取、未知序列丢弃、括号粘贴、Esc 超时）并按
// 模态 → 焦点 → 全局路由；打开带模态处理器的浮层接管按键，关闭后交还输入框。
BOOST_AUTO_TEST_CASE(keyboard_input_routes_through_focus_modal_and_global) {
    Child c = spawn_child([] {
        auto base = std::make_unique<Container>(Container::Direction::vertical);
        auto box = std::make_unique<InputBox>();
        InputBox* const box_p = box.get();
        base->add({Sizing::flex, 1}, std::move(box));
        LayerStack root{std::move(base)};
        Terminal term;
        Runtime rt{term, root};
        InputBoxHandler edit{*box_p};
        rt.set_focus(&edit, box_p);

        std::string global_log;
        std::string modal_log;
        uint32_t dialog = 0;
        Handler modal{[&](const Event& e) {
            if (is_key(e, Key::escape)) {
                modal_log += "<esc>";
                rt.close_overlay(dialog);
            } else if (e.kind == Event::Kind::text) {
                modal_log += e.text;
            }
            return true; // 模态吞掉一切
        }};
        Handler global{[&](const Event& e) {
            if (is_key(e, Key::enter)) {
                global_log += "<enter>";
                dialog = rt.open_overlay(text_widget("dialog"), Placement::center, {}, &modal);
                return true;
            }
            if (e.kind == Event::Kind::key && any(e.mods & Mods::alt)) {
                global_log += "<alt-" + e.text + ">";
                return true;
            }
            return false;
        }};
        rt.set_global(global);
        rt.run(); // stdin 关闭即退出

        if (box_p->text() != "你好Xp1\np2qa") ::_exit(3);
        if (global_log != "<alt-x><enter>") ::_exit(4);
        if (modal_log != "zz<esc>") ::_exit(5);
        ::_exit(0);
    });

    const std::string_view steps[] = {
        "\xe4\xbd",                        // 「你」的前两个字节
        "\xa0好ab",                        // 补齐
        "\x7f",                            // 退格 → 你好a
        "\x1b[D",                          // 左移到 a 前
        "X",                               // → 你好Xa
        "\x1b[?1;2;3z",                    // 认不出的序列：整体丢弃
        "\x1b[200~p1\np2\x1b[201~",        // 括号粘贴：换行只分行
        "\x1bx",                           // Alt+x：输入框不要 → 全局
        "\r",                              // Enter：全局打开模态浮层
        "zz",                              // 模态接管
        "\x1b",                            // 孤立 Esc：超时后成为 Esc 键，关闭浮层
        "q",                               // 回到输入框
    };
    for (const std::string_view s : steps) {
        send(c, s);
        std::this_thread::sleep_for(s == "\x1b" ? 150ms : 20ms);
    }
    BOOST_TEST(close_input_and_wait(c) == 0);
}

// 业务线程 post 不被慢帧阻塞：渲染线程还在画慢帧时，一万次 post 已全部返回；
// 慢帧结束后按提交顺序执行。
BOOST_AUTO_TEST_CASE(post_is_not_blocked_by_slow_frame) {
    struct Busy : Widget {
        std::atomic<bool> slow{false};
        std::atomic<bool> rendering{false};
        void render(Surface&) override {
            if (!slow.exchange(false)) return;
            rendering = true;
            const auto until = std::chrono::steady_clock::now() + 300ms;
            while (std::chrono::steady_clock::now() < until) {
            } // 不分配内存的忙等
            rendering = false;
        }
    };
    Child c = spawn_child([] {
        Busy root;
        Terminal term;
        Runtime rt{term, root};
        std::vector<int> seen;
        seen.reserve(10000);
        std::atomic<size_t> executed{0};
        int code = 0;
        std::thread business([&] {
            if (!wait_until([&] { return rt.frames() >= 1; })) code = 2;
            root.slow = true;
            rt.post([&] { root.invalidate(); });
            if (!wait_until([&] { return root.rendering.load(); })) code = 3;
            const uint64_t frames = rt.frames();
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 10000; ++i) {
                rt.post([&, i] {
                    seen.push_back(i);
                    executed.fetch_add(1);
                });
            }
            const auto spent = std::chrono::steady_clock::now() - t0;
            if (!root.rendering.load() || rt.frames() != frames) code = 4; // 慢帧仍在进行
            if (spent > 100ms) code = 5;
            if (executed.load() != 0) code = 6; // 业务线程上不执行
            if (!wait_until([&] { return executed.load() == 10000; })) code = 7;
            rt.quit();
        });
        rt.run();
        business.join();
        for (int i = 0; i < 10000 && code == 0; ++i) {
            if (seen[static_cast<size_t>(i)] != i) code = 8;
        }
        ::_exit(code);
    });
    BOOST_TEST(wait_exit_code(c) == 0);
}

// 定时器按到期顺序执行、可取消、every 返回 false 即停；定时器全部结束后，
// 没有输入与更新的运行时既不唤醒也不出帧。
BOOST_AUTO_TEST_CASE(timers_fire_in_order_then_idle_runtime_stays_silent) {
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        root.add({Sizing::flex, 1}, std::make_unique<Text>());
        Terminal term;
        Runtime rt{term, root};

        std::string order;
        int ticks = 0;
        int self_cancel_runs = 0;
        std::atomic<bool> done{false};
        // 建立顺序与到期顺序相反：按到期时刻执行，同时到期的按建立顺序。
        rt.after(60ms, [&] { order += 'c'; });
        rt.after(20ms, [&] { order += 'a'; });
        rt.after(40ms, [&] { order += 'b'; });
        rt.after(40ms, [&] { order += 'B'; });
        const TimerId dropped = rt.after(30ms, [&] { order += 'X'; });
        rt.cancel(dropped);
        rt.every(5ms, [&] { return ++ticks < 3; });
        TimerId self = 0;
        self = rt.every(5ms, [&] {
            ++self_cancel_runs;
            rt.cancel(self); // 回调里取消自己
            return true;
        });
        rt.after(70ms, [&] { rt.after(0ms, [&] { order += 'd'; }); });
        rt.after(100ms, [&] { done = true; });

        int code = 0;
        std::thread observer([&] {
            if (!wait_until([&] { return done.load(); })) code = 2;
            std::this_thread::sleep_for(30ms); // 最后一次唤醒落定
            const uint64_t w0 = rt.wakeups();
            const uint64_t f0 = rt.frames();
            std::this_thread::sleep_for(300ms);
            if (rt.wakeups() != w0) code = 3; // 零唤醒
            if (rt.frames() != f0) code = 4;  // 零帧
            rt.quit();
        });
        rt.run();
        observer.join();
        if (order != "abBcd") code = 5;
        if (ticks != 3) code = 6;
        if (self_cancel_runs != 1) code = 7;
        ::_exit(code);
    });
    BOOST_TEST(wait_exit_code(c) == 0);
}

// 快捷键：绑定串解析、单键与多键分派、leader 序列、enabled 条件；
// leader 后停手超时，后续按键回落到输入框。
BOOST_AUTO_TEST_CASE(keymap_binds_commands_and_leader_sequences) {
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        auto box = std::make_unique<InputBox>();
        InputBox* const box_p = box.get();
        root.add({Sizing::flex, 1}, std::move(box));
        Terminal term;
        Runtime rt{term, root};
        InputBoxHandler edit{*box_p};
        rt.set_focus(&edit, box_p);
        Keymap km{rt};
        rt.set_global(km);

        int a = 0, b = 0, f5 = 0, first = 0, second = 0, disabled = 0, shifted = 0, n = 0;
        km.add({"a", "A", "", [&] { ++a; }, {}});
        km.add({"b", "B", "", [&] { ++b; }, {}});
        km.add({"f5", "F5", "", [&] { ++f5; }, {}});
        km.add({"first", "X", "", [&] { ++first; }, {}});
        km.add({"second", "Y", "", [&] { ++second; }, {}});
        km.add({"off", "O", "", [&] { ++disabled; }, [] { return false; }});
        km.add({"shifted", "S", "", [&] { ++shifted; }, {}});
        km.add({"n", "N", "", [&] { ++n; }, {}});

        if (!km.bind("ctrl+shift+a", "a")) ::_exit(3);
        if (!km.bind("alt+enter", "b")) ::_exit(4);
        if (!km.bind("f5", "f5")) ::_exit(5);
        if (!km.bind("escape", "first") || !km.bind("escape", "second")) ::_exit(6);
        if (!km.bind("ctrl+q", "off")) ::_exit(7);
        if (!km.bind("shift+g", "shifted")) ::_exit(8);
        km.set_leader("ctrl+x", 80ms);
        if (!km.bind("<leader> n", "n")) ::_exit(9);
        for (const char* bad : {"", "ctrl+", "+a", "hyper+a", "nope", "f13"}) {
            if (km.bind(bad, "a")) ::_exit(10);
        }
        if (km.bind("ctrl+b", "missing")) ::_exit(11); // 命令不存在
        if (km.commands().size() != 8) ::_exit(12);    // 命令面板数据源

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
        // kitty 报出的 ctrl+shift+a 带大写字符，按小写 + shift 匹配。
        if (!km.on_event(chr("A", Mods::ctrl | Mods::shift)) || a != 1) ::_exit(20);
        if (!km.on_event(key(Key::enter, Mods::alt)) || b != 1) ::_exit(21);
        if (!km.on_event(key(Key::f5, Mods::none)) || f5 != 1) ::_exit(22);
        // 同一序列后绑定覆盖先绑定。
        if (!km.on_event(key(Key::escape, Mods::none)) || first != 0 || second != 1) ::_exit(23);
        // enabled 返回 false：按键被消费但不执行。
        if (!km.on_event(chr("q", Mods::ctrl)) || disabled != 0) ::_exit(24);
        // Shift+字母被终端报成大写文本；小写不匹配。
        if (!km.on_event(chr("G", Mods::none)) || shifted != 1) ::_exit(25);
        if (km.on_event(chr("g", Mods::none))) ::_exit(26);

        rt.run(); // 以下经真实字节驱动，stdin 关闭即退出
        if (n != 1) ::_exit(27);                // leader 后的 n 执行命令
        if (box_p->text() != "n") ::_exit(28);  // 超时后的 n 才进输入框
        ::_exit(0);
    });

    send(c, "\x18"); // ctrl+x：Keymap 自压模态，下一个键不进输入框
    std::this_thread::sleep_for(20ms);
    send(c, "n");
    std::this_thread::sleep_for(120ms);
    send(c, "\x18"); // 再按 leader 后停手，超过 80ms 超时
    std::this_thread::sleep_for(200ms);
    send(c, "n");
    std::this_thread::sleep_for(50ms);
    BOOST_TEST(close_input_and_wait(c) == 0);
}

// 鼠标选择与复制：拖拽跨行选择、双击选词、三击选行，松开即经 OSC 52 写入剪贴板，
// 内容是源文本。
BOOST_AUTO_TEST_CASE(mouse_selection_copies_source_text_to_clipboard) {
    Child c = spawn_pty_child([] {
        auto sb = std::make_unique<Scrollback>();
        Scrollback* const sb_p = sb.get();
        sb_p->document().append_block(BlockKind::text, "hello world\nsecond line");
        LayerStack root{std::move(sb)};
        Terminal term;
        term.set_mouse(true);
        Runtime rt{term, root};
        ScrollbackMouse mouse{rt, *sb_p};
        rt.bind_mouse(*sb_p, mouse);
        Handler quit{[&](const Event& e) {
            if (is_key(e, Key::enter)) rt.quit();
            return true;
        }};
        rt.set_global(quit);
        rt.run();
        ::_exit(0);
    });

    std::string out;
    BOOST_REQUIRE(read_until(c, out, "\x1b[c") != std::string::npos);
    send(c, "\x1b[?62;22c");
    BOOST_REQUIRE(read_until(c, out, "second") != std::string::npos);
    std::size_t pos = out.size();

    // 从 (0,0) 拖到第二行的 "second" 末尾并释放：跨硬换行。
    send(c, "\x1b[<0;1;1M\x1b[<32;4;2M\x1b[<32;6;2M\x1b[<0;6;2m");
    BOOST_TEST(next_clipboard(c, out, pos) == "hello world\nsecond");

    // 同一格双击 "world"。
    send(c, "\x1b[<0;9;1M\x1b[<0;9;1m\x1b[<0;9;1M\x1b[<0;9;1m");
    BOOST_TEST(next_clipboard(c, out, pos) == "world");

    // 等连击窗口过去，在第二行三击：第二下选词，第三下选整行。
    std::this_thread::sleep_for(500ms);
    send(c, "\x1b[<0;3;2M\x1b[<0;3;2m\x1b[<0;3;2M\x1b[<0;3;2m\x1b[<0;3;2M\x1b[<0;3;2m");
    BOOST_TEST(next_clipboard(c, out, pos) == "second");
    BOOST_TEST(next_clipboard(c, out, pos) == "second line");

    send(c, "\r");
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);
}

BOOST_AUTO_TEST_SUITE_END()
