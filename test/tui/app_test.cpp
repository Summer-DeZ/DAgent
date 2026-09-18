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
#include <vector>

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

// 固定尺寸方块：鼠标命中的目标控件。
class MouseBlock : public Widget {
public:
    MouseBlock(int w, int h, char32_t ch) : w_(w), h_(h), ch_(ch) {}
    Size measure(Size) const override { return {w_, h_}; }
    void render(Surface& s) override {
        s.fill({0, 0, s.cols(), s.rows()}, ch_, Style{});
    }
    std::optional<Point> cursor() const override { return Point{0, 0}; }

private:
    int w_;
    int h_;
    char32_t ch_;
};

// 记录鼠标事件（§3.5 用例的观察点）。
struct MouseRecorder : EventHandler {
    struct Seen {
        int button = -1;
        int x = 0;
        int y = 0;
        bool press = false;
        bool motion = false;
        bool outside = false;
    };
    std::vector<Seen> seen;

    bool on_event(const Event& e) override {
        if (e.kind != Event::Kind::mouse) return false;
        seen.push_back({e.mouse.button, e.mouse.x, e.mouse.y, e.mouse.press,
                        e.mouse.motion, e.mouse.outside});
        return true;
    }
};

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

// §3.5 验收（pty，父进程注入 SGR 鼠标序列）：
//   1. 滚轮落在滚动区坐标 → 滚动区处理器收到，输入框收不到；
//   2. 对话框打开时点击外部 → 基础层收不到，模态收到 outside = true；
//   3. 按下后拖出控件矩形并释放 → 移动与释放全部交给按下时的处理器。

BOOST_AUTO_TEST_CASE(mouse_wheel_goes_to_hit_widget_not_focus) {
    Child c = spawn_pty_child(
        [] {
            auto base = std::make_unique<Container>(Container::Direction::vertical);
            auto sb = std::make_unique<Scrollback>();
            Scrollback* sb_p = sb.get();
            base->add({Sizing::flex, 1}, std::move(sb));
            auto input = std::make_unique<InputBox>();
            InputBox* input_p = input.get();
            base->add({Sizing::fixed, 3}, std::move(input));
            LayerStack root{std::move(base)};

            MouseRecorder scroll_rec;
            MouseRecorder input_rec;

            Terminal term;
            Runtime rt{term, root};
            rt.set_focus(nullptr, input_p); // 焦点在输入框：鼠标不按焦点走
            rt.bind_mouse(*sb_p, scroll_rec);
            rt.bind_mouse(*input_p, input_rec);
            QuitOnEnter quit;
            quit.rt = &rt;
            rt.set_global(quit);
            rt.run();

            if (scroll_rec.seen.size() != 1) ::_exit(3);
            const auto& m = scroll_rec.seen[0];
            if (m.button != 4 || m.x != 10 || m.y != 4) ::_exit(4);
            if (!input_rec.seen.empty()) ::_exit(5);
            if (!input_p->text().empty()) ::_exit(6);
            ::_exit(0);
        },
        80, 24);

    std::string out;
    BOOST_REQUIRE(read_until(c, out, "\x1b[?25h")); // 首帧：raw 模式已就绪
    // 滚轮上（button 4）落在滚动区 (col 10, row 4)：SGR 坐标 1 基。
    BOOST_REQUIRE(::write(c.fd, "\x1b[<64;11;5M", 11) == 11);
    ::usleep(100 * 1000);
    BOOST_REQUIRE(::write(c.fd, "\r", 1) == 1);
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);
}

BOOST_AUTO_TEST_CASE(mouse_click_outside_modal_is_swallowed) {
    Child c = spawn_pty_child(
        [] {
            auto base = std::make_unique<Container>(Container::Direction::vertical);
            auto area = std::make_unique<MouseBlock>(40, 10, U'.');
            MouseBlock* area_p = area.get();
            base->add({Sizing::fixed, 10}, std::move(area));
            LayerStack root{std::move(base)};

            MouseRecorder base_rec;
            MouseRecorder modal;

            Terminal term;
            Runtime rt{term, root};
            rt.set_focus(nullptr, area_p); // 光标来源：仅用于帧就绪信号
            rt.bind_mouse(*area_p, base_rec);
            auto dialog = std::make_unique<MouseBlock>(20, 6, U'D');
            rt.open_overlay(std::move(dialog), Placement::center, {},
                            static_cast<EventHandler*>(&modal));
            QuitOnEnter quit;
            quit.rt = &rt;
            rt.set_global(quit);
            rt.run();

            // 对话框居中 {30,9,20,6}；点 (5,5) 在它外面、基础层方块内部。
            if (modal.seen.size() != 1) ::_exit(3);
            if (!modal.seen[0].outside || modal.seen[0].x != -25 ||
                modal.seen[0].y != -4) {
                ::_exit(4); // 坐标相对浮层左上角，可为负
            }
            if (!base_rec.seen.empty()) ::_exit(5); // 不穿透到基础层
            ::_exit(0);
        },
        80, 24);

    std::string out;
    BOOST_REQUIRE(read_until(c, out, "\x1b[?25h"));
    BOOST_REQUIRE(::write(c.fd, "\x1b[<0;6;6M", 9) == 9); // 左键按下 (5,5)
    ::usleep(100 * 1000);
    BOOST_REQUIRE(::write(c.fd, "\r", 1) == 1);
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);
}

BOOST_AUTO_TEST_CASE(mouse_drag_outside_is_captured_by_press_handler) {
    Child c = spawn_pty_child(
        [] {
            auto base = std::make_unique<Container>(Container::Direction::vertical);
            auto block = std::make_unique<MouseBlock>(10, 3, U'B');
            MouseBlock* block_p = block.get();
            base->add({Sizing::fixed, 3}, std::move(block));
            LayerStack root{std::move(base)};

            MouseRecorder rec;
            Terminal term;
            Runtime rt{term, root};
            rt.set_focus(nullptr, block_p); // 光标来源：仅用于帧就绪信号
            rt.bind_mouse(*block_p, rec);
            QuitOnEnter quit;
            quit.rt = &rt;
            rt.set_global(quit);
            rt.run();

            if (rec.seen.size() != 3) ::_exit(3);
            const auto& press = rec.seen[0];
            const auto& motion = rec.seen[1];
            const auto& release = rec.seen[2];
            if (!press.press || press.motion || press.x != 2 || press.y != 1) {
                ::_exit(4);
            }
            // 拖出矩形：移动与释放仍归按下时的处理器，坐标相对同一控件。
            if (!motion.press || !motion.motion || motion.x != 60 ||
                motion.y != 20) {
                ::_exit(5);
            }
            if (release.press || release.x != 60 || release.y != 20) ::_exit(6);
            if (press.button != 0 || motion.button != 0 || release.button != 0) {
                ::_exit(7);
            }
            ::_exit(0);
        },
        80, 24);

    std::string out;
    BOOST_REQUIRE(read_until(c, out, "\x1b[?25h"));
    // 一次写入三个事件：按下 (2,1) → 拖到 (60,20) → 在 (60,20) 释放。
    const char drag[] = "\x1b[<0;3;2M\x1b[<32;61;21M\x1b[<0;61;21m";
    BOOST_REQUIRE(::write(c.fd, drag, sizeof drag - 1) ==
                  static_cast<ssize_t>(sizeof drag - 1));
    ::usleep(100 * 1000);
    BOOST_REQUIRE(::write(c.fd, "\r", 1) == 1);
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);
}

BOOST_AUTO_TEST_SUITE_END()
