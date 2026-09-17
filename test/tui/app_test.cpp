// L7 运行时验证：真实线程 + 真实管道 / pty 的全链路（不是模拟）。
//   * 管道用例：子进程 stdin 接管道、stdout/stderr → /dev/null（Terminal 在
//     非 tty 下自动降级，尺寸回退 80×24），父进程注入按键/信号/EOF；
//   * pty 用例：子进程跑在 forkpty 的从端上（真实 tty：能改尺寸、收
//     SIGWINCH、输出真实转义序列），父进程持续读主端并解析输出。
// 子进程用退出码报告断言结果；alarm 兜底，死锁/空转的用例判失败。

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tui/app.hpp"
#include "tui/widget.hpp"

using namespace dagent::tui;
using namespace std::chrono_literals;

namespace {

struct Child {
    pid_t pid = -1;
    int fd = -1; // 管道用例：写入即子进程的键盘输入；pty 用例：主端
};

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
        ::alarm(10); // 卡死的用例直接判失败
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
        ::alarm(10);
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

BOOST_AUTO_TEST_CASE(burst_of_posts_is_coalesced_to_frame_rate) {
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
            // 约 1000 token/秒持续 320ms：最小帧间隔 16ms 下至多约 20 帧。
            const uint64_t before = rt.frames();
            const auto end = std::chrono::steady_clock::now() + 320ms;
            int i = 0;
            while (std::chrono::steady_clock::now() < end) {
                ++i;
                rt.post([log_p, i] { log_p->set_text("内容 " + std::to_string(i)); });
                std::this_thread::sleep_for(1ms);
            }
            std::this_thread::sleep_for(60ms); // 最后一批变更出帧
            const uint64_t produced = rt.frames() - before;
            if (produced < 4) code = 3;       // 突发期间持续出帧，没有被饿死
            if (produced > 320 / 16 + 4) code = 4; // 被合帧：远少于 post 次数
            if (i < 100) code = 5;            // 确实是突发
            std::string last;
            rt.post([&] { last = log_p->text(); });
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
    // 事件处理器与 tick 回调都在渲染线程的锁内运行，里面调 post()
    // 必须直接执行而不是重复加锁（死锁会被 alarm 杀掉，退出码 -1）。
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

                std::vector<Size> seen;
                rt.post([&] { seen = sizes.seen; });
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

BOOST_AUTO_TEST_SUITE_END()
