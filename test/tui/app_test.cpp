// L7 运行时验证：真实线程 + 真实管道 / pty 的全链路（不是模拟）。
//   * 管道用例：子进程 stdin 接管道、stdout/stderr → /dev/null（Terminal 在
//     非 tty 下自动降级，尺寸回退 80×24），父进程注入按键/信号/EOF；
//   * pty 用例：子进程跑在 forkpty 的从端上（真实 tty：能改尺寸、收
//     SIGWINCH、输出真实转义序列），父进程持续读主端并解析输出。
// 子进程用退出码报告断言结果；alarm 兜底，死锁/空转的用例判失败。

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tui/app.hpp"
#include "tui/document.hpp"
#include "tui/widget.hpp"

using namespace dagent::tui;
using namespace std::chrono_literals;

namespace {

struct Child {
    pid_t pid = -1;
    int fd = -1; // 管道用例：写入即子进程的键盘输入；pty 用例：主端
};

// 子进程的 alarm 兜底：用例只等调度与几个慢帧（毫秒级），10 秒足够把
// 死锁/空转判成失败。
constexpr unsigned k_alarm_s = 10;

// 子进程里：stdin 接管道读端、stdout/stderr → /dev/null、alarm 兜底。
// body 不返回（内部必须 _exit）。
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
        ::alarm(k_alarm_s); // 卡死的用例直接判失败
        body();
        ::_exit(99);
    }
    ::close(fds[0]);
    return {pid, fds[1]};
}

// 子进程跑在 cols×rows 的 pty 从端上。
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

// 轮询等待条件成立，超时返回 false。
template <class Pred>
bool wait_until(const Pred& pred, std::chrono::milliseconds limit = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
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



// 02 §3.1：post 入队即返回，fn 稍后在渲染线程上执行。需要同步读取
// 渲染线程状态的用例用 promise 在 fn 内 set_value，调用方 get() 等待。
template <class Fn>
auto post_sync(Runtime& rt, Fn&& fn) -> std::invoke_result_t<Fn&> {
    std::promise<std::invoke_result_t<Fn&>> p;
    auto fut = p.get_future();
    rt.post([&] { p.set_value(fn()); });
    return fut.get();
}

// render() 里忙等固定时长的控件：用它制造「渲染线程忙着」的慢帧。
// 为什么不用大文档的整树重折 —— 那要灌 64 MiB（-O0 下一帧 12 秒、RSS
// 300 MB），慢在制造条件而不是断言本身；而且大块 mmap/mremap 会持有
// 内核的地址空间写锁，业务线程只要有一次 malloc 需要扩堆就被挡住，
// 实测尖峰 10ms —— 那测的是内存子系统，不是框架的锁设计。忙等不分配，
// 慢帧时长可控，测出来的就是 post 自己的成本。
constexpr auto k_slow_frame = 300ms;
constexpr int64_t k_post_max_ns = 1'000'000; // 1ms

class SlowWidget : public Widget {
public:
    void render(Surface&) override {
        const auto until = std::chrono::steady_clock::now() + k_slow_frame;
        while (std::chrono::steady_clock::now() < until) {
            // 忙等：不分配、不进内核，纯粹占住渲染线程。
        }
    }
};

// 某个命名键触发退出并记录次数。
struct QuitOnKey : EventHandler {
    Runtime* rt = nullptr;
    Key trigger = Key::enter;
    int seen = 0;
    bool on_event(const Event& e) override {
        if (e.kind == Event::Kind::key && e.key == trigger) {
            ++seen;
            if (rt != nullptr) rt->quit();
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

// 打字 + 回车提交：焦点链把文本事件送进 InputBox，enter 下沉到全局。
void body_typing() {
    Container root{Container::Direction::vertical};
    auto input = std::make_unique<InputBox>();
    InputBox* input_p = input.get();
    InputBoxHandler handler(*input_p);

    Terminal term;
    Runtime rt{term, root};
    root.add({Sizing::flex, 1}, std::move(input));

    QuitOnKey quit_on_enter;
    quit_on_enter.rt = &rt;
    rt.set_focus(&handler, input_p);
    rt.set_global(quit_on_enter);
    rt.run();

    // enter 当帧即退出是合法行为（退出即还原终端），这里只断言内容。
    if (input_p->text() != "hi") ::_exit(3);
    ::_exit(0);
}

} // namespace

BOOST_AUTO_TEST_SUITE(app)

BOOST_AUTO_TEST_CASE(typing_reaches_input_box_and_enter_quits) {
    Child c = spawn_child(&body_typing);
    BOOST_REQUIRE(::write(c.fd, "hi\r", 3) == 3);
    BOOST_TEST(wait_exit_code(c.pid) == 0);
    ::close(c.fd);
}

BOOST_AUTO_TEST_CASE(lone_escape_times_out_into_escape_event) {
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        auto input = std::make_unique<InputBox>();
        InputBox* input_p = input.get();
        InputBoxHandler handler(*input_p);

        Terminal term;
        Runtime rt{term, root};
        root.add({Sizing::flex, 1}, std::move(input));

        QuitOnKey quit_on_escape;
        quit_on_escape.trigger = Key::escape;
        quit_on_escape.rt = &rt;
        rt.set_focus(&handler, input_p);
        rt.set_global(quit_on_escape);
        rt.run();

        ::_exit(quit_on_escape.seen == 1 ? 0 : 3);
    });
    // 孤立 ESC：解码器停在歧义窗口，由渲染线程的 40ms 超时消解。
    BOOST_REQUIRE(::write(c.fd, "\e", 1) == 1);
    BOOST_TEST(wait_exit_code(c.pid) == 0);
    ::close(c.fd);
}

BOOST_AUTO_TEST_CASE(thousand_posts_per_second_are_coalesced_into_frames) {
    // 验收（02 §3.1）：1000 次/秒 post 持续 1 秒 —— 帧数 ≤ 1000/16 + 余量，
    // 且最终内容正确（合帧不丢最后一次更新）。
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        auto log = std::make_unique<Text>();
        Text* log_p = log.get();
        root.add({Sizing::flex, 1}, std::move(log));

        Terminal term;
        Runtime rt{term, root, Runtime::Options{16ms, 100ms}};

        int code = 0;
        std::thread business([&] {
            if (!wait_until([&] { return rt.frames() >= 1; })) code = 2;
            const uint64_t before = rt.frames();
            const auto end = std::chrono::steady_clock::now() + 1s;
            int i = 0;
            while (std::chrono::steady_clock::now() < end) {
                ++i;
                rt.post([log_p, i] { log_p->set_text("内容 " + std::to_string(i)); });
                std::this_thread::sleep_for(1ms);
            }
            std::this_thread::sleep_for(60ms); // 最后一批变更出帧
            const uint64_t produced = rt.frames() - before;
            if (produced < 15) code = 3;             // 突发期间持续出帧，没有被饿死
            if (produced > 1000 / 16 + 16) code = 4; // 被合帧：远少于 post 次数
            if (i < 900) code = 5;                   // 确实是 1000 次/秒
            const std::string last = post_sync(rt, [&] { return log_p->text(); });
            if (last != "内容 " + std::to_string(i)) code = 6;
            rt.quit();
        });
        rt.run();
        business.join();
        ::_exit(code);
    });
    BOOST_TEST(wait_exit_code(c.pid) == 0);
    ::close(c.fd);
}

BOOST_AUTO_TEST_CASE(idle_ui_neither_ticks_nor_renders) {
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        root.add({Sizing::flex, 1}, std::make_unique<Text>());
        Terminal term;
        Runtime rt{term, root, Runtime::Options{16ms, 20ms}};

        std::atomic<int> ticks{0};
        rt.on_tick([&] {
            ticks.fetch_add(1);
            return false; // 没有动画：暂停 tick
        });

        int code = 0;
        std::thread driver([&] {
            if (!wait_until([&] { return ticks.load() >= 1 && rt.frames() >= 1; })) code = 2;
            const int t0 = ticks.load();
            const uint64_t f0 = rt.frames();
            // 20ms 的 tick 周期下静置 300ms：常挂 tick 会触发约 15 次。
            std::this_thread::sleep_for(300ms);
            if (ticks.load() != t0) code = 3;
            if (rt.frames() != f0) code = 4; // 静止界面零帧

            // post 可能启动了动画：tick 重新挂上一次，回调返回 false 后再暂停。
            rt.post([] {});
            if (!wait_until([&] { return ticks.load() == t0 + 1; }, 500ms)) code = 5;
            std::this_thread::sleep_for(150ms);
            if (ticks.load() != t0 + 1) code = 6;
            if (rt.frames() != f0) code = 7; // 空 post 不失效任何控件 → 不出帧
            rt.quit();
        });
        rt.run();
        driver.join();
        ::_exit(code);
    });
    BOOST_TEST(wait_exit_code(c.pid) == 0);
    ::close(c.fd);
}

BOOST_AUTO_TEST_CASE(post_from_render_thread_code_does_not_deadlock) {
    // 事件处理器与 tick 回调都在渲染线程上运行，里面调 post() 必须
    // 直接执行而不是入队等自己（死锁会被 alarm 杀掉，退出码 -1）。
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        auto log = std::make_unique<Text>();
        Text* log_p = log.get();
        root.add({Sizing::flex, 1}, std::move(log));
        Terminal term;
        Runtime rt{term, root, Runtime::Options{16ms, 20ms}};

        int tick_posts = 0;
        rt.on_tick([&] {
            rt.post([&] { ++tick_posts; });
            return false;
        });

        struct PostOnKey : EventHandler {
            Runtime* rt = nullptr;
            Text* log = nullptr;
            bool on_event(const Event& e) override {
                if (e.kind != Event::Kind::text) return false;
                rt->post([this] { log->set_text("posted"); });
                rt->quit();
                return true;
            }
        } handler;
        handler.rt = &rt;
        handler.log = log_p;
        rt.set_global(handler);

        rt.run();
        if (log_p->text() != "posted") ::_exit(3);
        if (tick_posts < 1) ::_exit(4);
        ::_exit(0);
    });
    ::usleep(100 * 1000); // 让首个 tick 先跑完
    BOOST_REQUIRE(::write(c.fd, "a", 1) == 1);
    BOOST_TEST(wait_exit_code(c.pid) == 0);
    ::close(c.fd);
}

BOOST_AUTO_TEST_CASE(idle_resize_repaints_and_routes_resize_events) {
    // 空闲界面（无动画、无输入）下改变 pty 尺寸：SIGWINCH 唤醒渲染线程，
    // 立即补画并下发 resize 事件；首帧从未知尺寸到 80×24 也下发一次。
    Child c = spawn_pty_child(
        [] {
            Container root{Container::Direction::vertical};
            auto text = std::make_unique<Text>();
            text->set_text("hello");
            root.add({Sizing::flex, 1}, std::move(text));

            struct Sizes : EventHandler {
                std::vector<Size> seen;
                bool on_event(const Event& e) override {
                    if (e.kind == Event::Kind::resize) seen.push_back(e.size);
                    return false;
                }
            } sizes;

            Terminal term;
            Runtime rt{term, root};
            rt.set_global(sizes);

            int code = 0;
            std::thread driver([&] {
                if (!wait_until([&] { return rt.frames() >= 1; })) code = 2;
                std::this_thread::sleep_for(100ms);
                const uint64_t f0 = rt.frames();

                winsize ws{};
                ws.ws_col = 100;
                ws.ws_row = 30;
                ::ioctl(STDOUT_FILENO, TIOCSWINSZ, &ws);
                if (!wait_until([&] { return rt.frames() > f0; }, 1000ms)) code = 3;

                const std::vector<Size> seen =
                    post_sync(rt, [&] { return sizes.seen; });
                const std::vector<Size> expected{{80, 24}, {100, 30}};
                if (code == 0 && seen != expected) code = 4;
                rt.quit();
            });
            rt.run();
            driver.join();
            ::_exit(code);
        },
        80, 24);
    std::string out;
    BOOST_TEST(drain_pty_until_exit(c, out) == 0);
}

BOOST_AUTO_TEST_CASE(nested_focus_cursor_is_placed_in_screen_coordinates) {
    // 80×24：上方 flex 文本，底部固定 3 行的横向容器 = 4 列提示符 + 输入框。
    // 输入框局部原点在容器内 (4,0)，容器在屏幕 (0,21)；空输入框光标在
    // 边框内 (1,1) → 屏幕 (5,22) → CUP 1 基 "\e[23;6H"。
    Child c = spawn_pty_child(
        [] {
            Container root{Container::Direction::vertical};
            root.add({Sizing::flex, 1}, std::make_unique<Text>());
            auto bar = std::make_unique<Container>(Container::Direction::horizontal);
            auto prompt = std::make_unique<Text>();
            prompt->set_text(">>> ");
            bar->add({Sizing::fixed, 4}, std::move(prompt));
            auto input = std::make_unique<InputBox>();
            InputBox* input_p = input.get();
            bar->add({Sizing::flex, 1}, std::move(input));
            root.add({Sizing::fixed, 3}, std::move(bar));
            InputBoxHandler handler(*input_p);

            Terminal term;
            Runtime rt{term, root};
            rt.set_focus(&handler, input_p);
            std::thread driver([&] {
                wait_until([&] { return rt.frames() >= 1; });
                rt.quit();
            });
            rt.run();
            driver.join();
            ::_exit(0);
        },
        80, 24);
    std::string out;
    BOOST_REQUIRE(drain_pty_until_exit(c, out) == 0);

    // 帧末光标定位：最后一个"显示光标"之前紧挨着的 CUP。
    const std::size_t show = out.rfind("\x1b[?25h");
    BOOST_REQUIRE(show != std::string::npos);
    const std::size_t cup = out.rfind("\x1b[", show - 1);
    BOOST_REQUIRE(cup != std::string::npos);
    BOOST_TEST(out.substr(cup, show - cup) == "\x1b[23;6H");
}

BOOST_AUTO_TEST_CASE(post_stays_sub_millisecond_while_render_thread_is_busy) {
    // 验收（02 §3.1）：渲染线程被一个慢帧占住期间，业务线程连续 post 的
    // 单次耗时 < 1ms —— post 的临界区只有一次尾插（最坏 O(1)，与已积压的
    // 队列长度无关），与渲染耗时无关。旧设计里 post 与渲染共用 state_ 锁，
    // 这里的 max 会等于整个慢帧时长。
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        auto slow = std::make_unique<SlowWidget>();
        SlowWidget* slow_p = slow.get();
        root.add({Sizing::flex, 1}, std::move(slow));

        Terminal term;
        Runtime rt{term, root, Runtime::Options{16ms, 100ms}};

        int code = 0;
        std::thread business([&] {
            if (!wait_until([&] { return rt.frames() >= 1; })) {
                code = 2;
                rt.quit();
                return;
            }
            const uint64_t f0 = rt.frames();
            rt.post([slow_p] { slow_p->invalidate(); }); // 触发下一个慢帧

            // 样本容量一次配够：记录本身（vector 扩容）不能干扰测量。
            std::vector<int64_t> lat_ns;
            lat_ns.reserve(1u << 16);
            const auto start = std::chrono::steady_clock::now();
            while (rt.frames() == f0) {
                if (std::chrono::steady_clock::now() - start > 10s) {
                    code = 3; // 慢帧没有出现
                    break;
                }
                const auto a = std::chrono::steady_clock::now();
                rt.post([] {});
                const auto b = std::chrono::steady_clock::now();
                if (lat_ns.size() < lat_ns.capacity()) {
                    lat_ns.push_back(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a)
                            .count());
                }
                std::this_thread::sleep_for(100us);
            }
            const auto frame_at = std::chrono::steady_clock::now();
            if (code == 0 && frame_at - start < 50ms) code = 4; // 慢帧确实发生
            if (code == 0 && lat_ns.size() < 100) code = 5;     // 窗口覆盖足够密
            if (code == 0 &&
                *std::max_element(lat_ns.begin(), lat_ns.end()) >= k_post_max_ns) {
                code = 6; // 验收核心断言
            }
            rt.quit();
        });
        rt.run();
        business.join();
        ::_exit(code);
    });
    BOOST_TEST(wait_exit_code(c.pid) == 0);
    ::close(c.fd);
}

BOOST_AUTO_TEST_CASE(sigterm_exits_gracefully) {
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        Terminal term;
        Runtime rt{term, root};
        rt.run(); // SIGTERM → self-pipe → 正常退出路径
        ::_exit(0);
    });
    ::usleep(150 * 1000); // 让子进程进入 poll
    ::kill(c.pid, SIGTERM);
    BOOST_TEST(wait_exit_code(c.pid) == 0);
    ::close(c.fd);
}

BOOST_AUTO_TEST_CASE(tick_drives_animation_and_quit) {
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        Terminal term;
        Runtime rt{term, root, Runtime::Options{16ms, 40ms}};

        std::atomic<int> ticks{0};
        rt.on_tick([&] {
            if (ticks.fetch_add(1) + 1 >= 3) rt.quit(); // 第 3 个 tick 退出
            return true;                                 // 动画进行中：持续 tick
        });
        rt.run();

        ::_exit(ticks.load() >= 3 ? 0 : 3);
    });
    BOOST_TEST(wait_exit_code(c.pid) == 0);
    ::close(c.fd);
}

BOOST_AUTO_TEST_CASE(stdin_eof_quits) {
    Child c = spawn_child([] {
        Container root{Container::Direction::vertical};
        Terminal term;
        Runtime rt{term, root};
        rt.run();
        ::_exit(0);
    });
    ::close(c.fd); // EOF：终端关闭，正常收摊
    BOOST_TEST(wait_exit_code(c.pid) == 0);
}

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
