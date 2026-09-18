// M4 §3.4.3 浮层与光标来源：子进程跑在 forkpty 的从端上（真实 tty），
// 父进程读主端的帧输出、注入按键。子进程用退出码报告断言结果；alarm
// 兜底，死锁/空转的用例判失败。

#include <boost/test/unit_test.hpp>

#include <cerrno>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <poll.h>
#include <pty.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tui/app.hpp"
#include "tui/layout.hpp"
#include "tui/widget.hpp"

using namespace dagent::tui;
using namespace std::chrono_literals;

namespace {

struct Child {
    pid_t pid = -1;
    int fd = -1; // pty 主端
};

// 子进程的 alarm 兜底：用例只等几帧（毫秒级），10 秒足够把
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

// 光标来源：固定 10×1、光标恒在左上角（InputBox 带边框，空内容时
// 量出来放不下光标）。
class CursorBlock : public Widget {
public:
    Size measure(Size) const override { return {10, 1}; }
    void render(Surface& s) override {
        s.fill({0, 0, s.cols(), s.rows()}, U'#', Style{});
    }
    std::optional<Point> cursor() const override { return Point{0, 0}; }
};

// 最后一帧定位光标的行号（1 基）：帧末是 CUP + \e[?25h。
int last_cursor_row(std::string_view out) {
    const std::size_t show = out.rfind("\x1b[?25h");
    if (show == std::string_view::npos) return -1;
    const std::size_t cup = out.rfind("\x1b[", show - 1);
    if (cup == std::string_view::npos) return -1;
    int row = 0;
    for (std::size_t i = cup + 2; i < show && out[i] >= '0' && out[i] <= '9'; ++i) {
        row = row * 10 + (out[i] - '0');
    }
    return row;
}

} // namespace

BOOST_AUTO_TEST_SUITE(app)

// 非后进先出关闭带光标来源的浮层：先开 A、再开 B，先关 A 再关 B。
// B 记下的恢复点是 A 的光标来源，而 A 关闭时控件已销毁 —— 恢复点必须
// 改接到 A 自己的恢复点（基础层光标来源，第 1 行），不能悬垂。
BOOST_AUTO_TEST_CASE(non_lifo_overlay_close_restores_base_cursor) {
    Child c = spawn_pty_child(
        [] {
            auto base = std::make_unique<Container>(Container::Direction::vertical);
            auto field = std::make_unique<CursorBlock>();
            Widget* base_cursor = field.get();
            base->add({Sizing::fixed, 1}, std::move(field));
            LayerStack root{std::move(base)};
            Terminal term;
            Runtime rt{term, root};
            rt.set_focus(nullptr, base_cursor);

            auto a = std::make_unique<CursorBlock>();
            Widget* a_cursor = a.get();
            auto b = std::make_unique<CursorBlock>();
            Widget* b_cursor = b.get();
            const uint32_t id_a = rt.open_overlay(std::move(a), Placement::at_point,
                                                  {0, 5}, nullptr, a_cursor);
            const uint32_t id_b = rt.open_overlay(std::move(b), Placement::at_point,
                                                  {0, 8}, nullptr, b_cursor);

            // 第一次回车：按 A、B 的顺序关闭；第二次回车：退出。
            struct Closer : EventHandler {
                Runtime* rt = nullptr;
                uint32_t first = 0;
                uint32_t second = 0;
                bool closed = false;
                bool on_event(const Event& e) override {
                    if (e.kind != Event::Kind::key || e.key != Key::enter) return false;
                    if (closed) {
                        rt->quit();
                    } else {
                        rt->close_overlay(first);
                        rt->close_overlay(second);
                        closed = true;
                    }
                    return true;
                }
            } closer;
            closer.rt = &rt;
            closer.first = id_a;
            closer.second = id_b;
            rt.set_global(closer);
            rt.run();
            ::_exit(0);
        },
        80, 24);

    std::string out;
    BOOST_REQUIRE(read_until(c, out, "\x1b[?25h")); // 首帧：光标在 B（第 9 行）
    BOOST_TEST(last_cursor_row(out) == 9);

    out.clear();
    BOOST_REQUIRE(::write(c.fd, "\r", 1) == 1);
    BOOST_REQUIRE(read_until(c, out, "\x1b[?25h")); // 关闭后的帧
    BOOST_TEST(last_cursor_row(out) == 1);          // 回到基础层光标来源

    BOOST_REQUIRE(::write(c.fd, "\r", 1) == 1);
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);
}

BOOST_AUTO_TEST_SUITE_END()
