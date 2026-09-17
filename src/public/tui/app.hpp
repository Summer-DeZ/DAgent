// L7 运行时：调度 —— 置脏 → 合帧 → 单线程渲染（文档§十）。
//
// 线程模型：
//   * 业务线程（任意多个）：post() 在 state_mutex 内改 widget 状态 ——
//     只做纯内存操作（零 I/O、O(变化量)），随后置脏并唤醒渲染。
//   * 渲染线程 = 调用 run() 的线程，全框架唯一的终端 I/O 者：
//     poll stdin / 信号 self-pipe / 唤醒管道 → 解码输入 → 按焦点链
//     路由（锁内纯内存）→ 探测尺寸、必要时提升布局纪元 → 把失效控件
//     光栅化到 back_ → 解锁后才差分写出。state_mutex 内绝不做 I/O
//     （终端写入慢时持锁会把延迟回压到业务线程）。
//   * 渲染线程在锁内调用应用代码（事件处理器、tick 回调、widget 的
//     render）。这些代码里调用 post() 是合法的：识别出渲染线程后直接
//     执行，不重复加锁。
//
// 帧节奏是被唤醒驱动而不是固定频率轮询：按键与业务变更立即出帧；
// 最小帧间隔（默认 16ms）只用来合并突发（一千 token/秒 ≈ 60 帧）。
// 只有控件树真的失效（或尺寸/焦点变化）才出帧。
//
// 动画 tick（默认 100ms）只在动画进行时挂着：tick 回调返回 false 即暂停，
// 下一次 post() 或输入事件（可能启动了新动画）重新挂上。静止界面零输出、
// 零唤醒 —— poll 无限期阻塞，直到真的有事发生。
//
// 等待点用唤醒管道而不是条件变量：渲染线程要同时等文件描述符
// （stdin / self-pipe）与业务线程的置脏通知，单线程里 poll 与
// cv 不能同时等待，管道把两者统一进 poll。突发的 post 只写一个字节。
//
// 尺寸：SIGWINCH 经 L1 的 self-pipe 唤醒（信号上下文只写一个字节），
// 渲染线程随即 ioctl 读尺寸；每帧开头也再查一次兜底。尺寸变化时
// resize 纪元 + 整树补画，并经路由下发 Kind::resize 事件（首帧从
// 未知尺寸到实际尺寸也算一次）。SIGINT/SIGTERM/SIGHUP 转成正常退出路径；
// SIGKILL/SIGSEGV 无法覆盖。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "tui/input.hpp"
#include "tui/terminal.hpp"
#include "tui/widget.hpp"

namespace dagent::tui {

class Runtime {
public:
    struct Options {
        std::chrono::milliseconds min_frame{16}; // 合帧窗口（≈60fps 上限）
        std::chrono::milliseconds tick{100};     // 动画 tick 周期
    };

    // root 是整棵控件树的根（非拥有）。run() 期间它只在 state_mutex 内被触碰。
    Runtime(Terminal& term, Widget& root, Options opt);
    Runtime(Terminal& term, Widget& root);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // ---- 配置（run() 之前，或在渲染线程的应用代码内调用）----
    // 焦点有两副面孔：路由目标（L6 处理器）与光标来源（widget 的
    // cursor() 加上 screen_origin() 成屏幕坐标，帧末定位）。
    void set_focus(EventHandler* routing, Widget* cursor_source = nullptr) noexcept {
        router_.set_focus(routing);
        cursor_source_ = cursor_source;
        dirty_ = true; // 光标来源变了：即使控件都没失效也要重新定位
    }
    void set_global(EventHandler& h) noexcept { router_.set_global(&h); }
    void push_modal(EventHandler& h) { router_.push(h); }
    void pop_modal(EventHandler& h) { router_.pop(h); }

    // 动画 tick 回调，在 state_mutex 内调用：推进动画帧并 invalidate。
    // 返回 true = 动画仍在进行，继续下一个 tick；返回 false = 暂停，
    // 直到下一次 post() 或输入事件重新挂上。
    void on_tick(std::function<bool()> fn);

    // ---- 业务线程（任意线程，含渲染线程上的应用代码）----
    // fn 在 state_mutex 内执行，禁 I/O。
    void post(std::function<void()> fn);

    // 请求退出（任意线程；只置原子标志并唤醒等待点）。
    void quit() noexcept;

    // 把调用线程变成渲染线程。阻塞直到 quit、退出类信号或 stdin 关闭。
    void run();

    // 已产出的帧数（诊断与验证用；稳态下增速即帧率）。
    uint64_t frames() const noexcept { return frames_.load(std::memory_order_relaxed); }

private:
    using Clock = std::chrono::steady_clock;

    // 以下 *_locked 都必须在 state_mutex 内（或渲染线程已持锁时）调用。
    void route_locked();                         // 路由 events_ 并清空
    void check_size_locked();                    // ioctl 尺寸；变了则纪元 + resize 事件
    void frame_locked();                         // 尺寸 → 布局 → 光栅化 → 光标
    void note_changes_locked() noexcept;         // 控件树失效 → 需要出帧
    void arm_tick_locked() noexcept;             // 可能启动了动画：挂上 tick
    int poll_timeout_locked(Clock::time_point now) const noexcept; // -1 = 无限期
    bool on_render_thread() const noexcept;
    void wake() noexcept;
    void drain_wake() noexcept;

    Terminal& term_;
    Widget& root_;
    Options opt_;

    EventRouter router_;
    Decoder decoder_;

    // ---- state_mutex 保护（含控件树与下列状态）----
    std::mutex state_;
    bool dirty_ = true;   // 需要出帧（首帧）
    bool ticking_ = false; // tick 是否挂着
    std::optional<Clock::time_point> esc_due_; // Esc 歧义超时的截止时刻
    Clock::time_point next_tick_{};
    Widget* cursor_source_ = nullptr;
    std::function<bool()> tick_fn_;
    std::vector<Event> events_; // 复用：解码事件的缓冲
    Size size_{};               // 已知终端尺寸（{0,0} = 未知）

    // ---- 跨线程原子 ----
    std::atomic<bool> quit_{false};
    std::atomic<bool> wake_pending_{false}; // 管道里已有未消费的唤醒字节
    std::atomic<uint64_t> frames_{0};
    std::atomic<std::thread::id> render_thread_{};

    // ---- 仅渲染线程触碰（锁外）----
    Clock::time_point last_frame_{};
    Surface back_;
    Surface front_;
    std::string out_; // 复用容量：稳态帧路径零分配
    std::optional<Point> cursor_;
    int wake_pipe_[2] = {-1, -1}; // post()/quit() 唤醒 poll 的管道
};

} // namespace dagent::tui
