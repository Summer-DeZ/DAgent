// M2 §3.3 能力握手验收：子进程跑在 forkpty 的从端上（真实 tty），
// 父进程在主端扮演终端 —— 读取查询、按用例写回应答，再注入按键。
// 子进程用退出码报告断言结果；alarm 兜底，死锁/空转的用例判失败。

#include <boost/test/unit_test.hpp>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <poll.h>
#include <pty.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tui/app.hpp"
#include "tui/widget.hpp"

using namespace dagent::tui;
using namespace std::chrono_literals;

namespace {

struct Child {
    pid_t pid = -1;
    int fd = -1; // pty 主端
};

// 子进程的 alarm 兜底：最长的用例等 1 秒握手超时，10 秒足够把
// 死锁/空转判成失败。
constexpr unsigned k_alarm_s = 10;

// 子进程跑在 cols×rows 的 pty 从端上。body 不返回（内部必须 _exit）。
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

// 持续读 pty 主端直到 needle 出现（用于在子进程运行中扮演终端）。
bool read_until(Child& c, std::string& out, std::string_view needle,
                std::chrono::milliseconds limit = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (out.find(needle) == std::string::npos) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        pollfd p{c.fd, POLLIN, 0};
        ::poll(&p, 1, 20);
        if ((p.revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
        char buf[4096];
        const ssize_t n = ::read(c.fd, buf, sizeof buf);
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0) {
            return false;
        } else if (errno != EINTR && errno != EAGAIN) {
            return false;
        }
    }
    return true;
}

// 回车退出：父进程写完输入后以 \r 收尾，子进程据此结束 run()。
struct QuitOnKey : EventHandler {
    Runtime* rt = nullptr;
    bool on_event(const Event& e) override {
        if (e.kind == Event::Kind::key && e.key == Key::enter) {
            rt->quit();
            return true;
        }
        return false;
    }
};

// 握手 pty 用例的子进程侧脚手架：最小输入界面 + Runtime，父进程退出后
// 按退出码报告断言结果。
struct HandshakeUi {
    Container root{Container::Direction::vertical};
    std::unique_ptr<InputBox> box = std::make_unique<InputBox>();
    InputBox* input = box.get();
    InputBoxHandler handler{*input};
    Terminal term;
    Runtime rt{term, root};
    QuitOnKey quit_on_enter;

    HandshakeUi() {
        root.add({Sizing::flex, 1}, std::move(box));
        quit_on_enter.rt = &rt;
        rt.set_focus(&handler, input);
        rt.set_global(quit_on_enter);
    }

    [[nodiscard]] bool caps_unchanged(const Terminal::Caps& initial) const {
        const Terminal::Caps& c = term.caps();
        return c.synchronized == initial.synchronized &&
               c.grapheme_width == initial.grapheme_width &&
               c.kitty_keyboard == initial.kitty_keyboard &&
               c.background.has_value() == initial.background.has_value();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(app)

// §3.3 验收（pty，父进程扮演终端）：
//   1. 回全部应答 → caps 升级 + 主端收到 \e[>1u + 输入框无杂字；
//   2. 只回 DA1 → caps 保持初始值，窗口关闭后 \e] 仍是 Alt-]；
//   3. 不回应 → 1 秒后窗口关闭，其间与之后的输入正常。

BOOST_AUTO_TEST_CASE(handshake_full_replies_upgrade_caps_and_enable_kitty) {
    Child c = spawn_pty_child(
        [] {
            HandshakeUi ui;
            ui.rt.run();

            const Terminal::Caps& caps = ui.term.caps();
            if (!caps.synchronized) ::_exit(3);
            if (!caps.grapheme_width) ::_exit(4);
            if (!caps.kitty_keyboard) ::_exit(5);
            if (!caps.background || caps.background->kind != Color::Kind::rgb ||
                caps.background->r != 0x1e || caps.background->g != 0x1e ||
                caps.background->b != 0x1e) {
                ::_exit(6);
            }
            if (ui.input->text() != "hi") ::_exit(7); // 应答没有泄漏成文本
            ui.term.restore(); // _exit 不跑析构：显式还原，弹出 kitty flag
            ::_exit(0);
        },
        80, 24);

    std::string out;
    // 子进程 run() 开始即发出全部查询（DA1 最后）。
    BOOST_REQUIRE(read_until(
        c, out,
        "\x1b[?2026$p\x1b[?2027$p\x1b[?u\x1b]11;?\x1b\\\x1b[c"));
    const std::string replies =
        "\x1b[?2026;2$y"                     // 同步输出支持
        "\x1b[?2027;2$y"                     // 字素簇宽度支持
        "\x1b[?1u"                           // kitty flags（协议支持）
        "\x1b]11;rgb:1e1e/1e1e/1e1e\x1b\\"   // 背景色
        "\x1b[?62;22c";                      // DA1 哨兵
    BOOST_REQUIRE(::write(c.fd, replies.data(), replies.size()) ==
                  static_cast<ssize_t>(replies.size()));
    // caps 升级后推入 kitty flag 1（\e[>1u）。
    BOOST_REQUIRE(read_until(c, out, "\x1b[>1u"));
    BOOST_REQUIRE(::write(c.fd, "hi\r", 3) == 3);
    BOOST_TEST(read_until(c, out, "\x1b[<u")); // 还原路径逆序弹出 flag
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);
}

BOOST_AUTO_TEST_CASE(handshake_unsupported_reply_overrides_env_guess) {
    // 环境变量只是初始值：明确的“不支持”应答（DECRQM value 0）必须
    // 覆盖乐观猜测（TERM 含 kitty → synchronized 初始为 true）。
    Child c = spawn_pty_child(
        [] {
            ::setenv("TERM", "xterm-kitty", 1);
            ::unsetenv("COLORTERM");
            HandshakeUi ui;
            if (!ui.term.caps().synchronized) ::_exit(3);
            ui.rt.run();
            if (ui.term.caps().synchronized) ::_exit(4);
            ::_exit(0);
        },
        80, 24);

    std::string out;
    BOOST_REQUIRE(read_until(c, out, "\x1b[c"));
    const std::string replies = "\x1b[?2026;0$y\x1b[?62;22c";
    BOOST_REQUIRE(::write(c.fd, replies.data(), replies.size()) ==
                  static_cast<ssize_t>(replies.size()));
    BOOST_REQUIRE(::write(c.fd, "\r", 1) == 1);
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);
}

BOOST_AUTO_TEST_CASE(handshake_only_da1_keeps_initial_caps) {
    Child c = spawn_pty_child(
        [] {
            HandshakeUi ui;
            const Terminal::Caps initial = ui.term.caps();

            struct Probe : EventHandler {
                Runtime* rt = nullptr;
                bool saw_alt_bracket = false;
                bool on_event(const Event& e) override {
                    if (e.kind == Event::Kind::key && e.key == Key::enter) {
                        rt->quit();
                        return true;
                    }
                    if (e.kind == Event::Kind::key &&
                        any(e.mods & Mods::alt) && e.text == "]") {
                        saw_alt_bracket = true;
                        return true;
                    }
                    return false;
                }
            } probe;
            probe.rt = &ui.rt;
            ui.rt.set_global(probe);
            ui.rt.run();

            if (!ui.caps_unchanged(initial)) ::_exit(3);
            if (!probe.saw_alt_bracket) ::_exit(4); // 窗口已关：\e] 仍是 Alt-]
            if (ui.input->text() != "x") ::_exit(5); // 没被当成应答吞掉
            ::_exit(0);
        },
        80, 24);

    std::string out;
    BOOST_REQUIRE(read_until(c, out, "\x1b[c"));
    const std::string da1 = "\x1b[?62;22c";
    BOOST_REQUIRE(::write(c.fd, da1.data(), da1.size()) ==
                  static_cast<ssize_t>(da1.size()));
    std::this_thread::sleep_for(100ms); // 等子进程消费 DA1、关闭应答窗口
    BOOST_REQUIRE(::write(c.fd, "\x1b]x\r", 4) == 4);
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);
}

BOOST_AUTO_TEST_CASE(handshake_timeout_keeps_input_working) {
    Child c = spawn_pty_child(
        [] {
            HandshakeUi ui;
            const Terminal::Caps initial = ui.term.caps();
            ui.rt.run();

            if (!ui.caps_unchanged(initial)) ::_exit(3);
            if (ui.input->text() != "ab") ::_exit(4); // 窗口内与之后的输入都正常
            ::_exit(0);
        },
        80, 24);

    std::string out;
    BOOST_REQUIRE(read_until(c, out, "\x1b[c"));
    BOOST_REQUIRE(::write(c.fd, "a", 1) == 1); // 应答窗口打开期间的输入
    std::this_thread::sleep_for(1300ms);        // 1 秒超时：窗口关闭、能力保持初始值
    BOOST_REQUIRE(::write(c.fd, "b\r", 2) == 2);
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);
}

BOOST_AUTO_TEST_SUITE_END()
