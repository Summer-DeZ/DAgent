// L7 运行时：调度 —— 置脏 → 合帧 → 单线程渲染（文档§十）。
//
// 线程模型（02-tui-final-update §3.1：业务线程零阻塞）：
//   * 控件树与 Document 只属于渲染线程，框架里没有保护它们的锁。
//     渲染线程 = 调用 run() 的线程，全框架唯一的终端 I/O 者：
//     poll stdin / 信号 self-pipe / 唤醒管道 → 交换更新队列并依序
//     执行 → 解码输入、按焦点链路由 → 探测尺寸、必要时提升布局纪元
//     → 把失效控件光栅化到 back_ → 差分写出。
//   * 业务线程（任意多个）与渲染线程之间只有一个更新队列：post() 的
//     临界区只有一次尾插，最坏情况 O(1)，与渲染耗时和队列长度都无关
//     —— 大文档重排期间业务线程也不会被等锁。fn 入队即返回，稍后在渲染线程上执行
//     （不是在调用线程上）；需要完成通知的调用方在 fn 内 set_value
//     一个 promise，自己 get() 等待。渲染线程上的应用代码（事件
//     处理器、定时器回调、widget 的 render）调用 post() 时直接执行，
//     保持重入语义。
//   * set_focus / after / every 等配置接口的线程约束：run() 之前或渲染
//     线程；业务线程经 post 间接调用。
//
// 关键纪律：队列锁 queue_mutex_ 内只有指针改写。
// 业务线程入队：锁外分配节点 → 锁 queue_mutex_ → 尾插（改两个指针）→
// 解锁 → 唤醒。渲染线程每次被唤醒：锁 queue_mutex_ → 摘走整条链
// （置空头尾）→ 解锁 → 依序执行并释放节点 → 检查控件树是否失效。
// 入队先于唤醒、清标志先于摘链，两者配对保证不丢唤醒。
//
// 帧节奏是被唤醒驱动而不是固定频率轮询：按键与业务变更立即出帧；
// 最小帧间隔（默认 16ms）只用来合并突发（一千 token/秒 ≈ 60 帧）。
// 只有控件树真的失效（或尺寸/焦点变化）才出帧。
//
// 定时器（§3.9）：after / every 的到期时刻存最小堆，poll 超时取堆顶、
// Esc 超时、握手超时、合帧余量的最小值。动画用 every，回调返回 false
// 即停止；启动动画的代码显式调用 every。堆空且无其他等待时 poll 无限期
// 阻塞：静止界面零输出、零唤醒。
//
// 等待点用唤醒管道而不是条件变量：渲染线程要同时等文件描述符
// （stdin / self-pipe）与业务线程的更新通知，单线程里 poll 与
// cv 不能同时等待，管道把两者统一进 poll。突发的 post 只写一个字节。
//
// 尺寸：SIGWINCH 经 L1 的 self-pipe 唤醒（信号上下文只写一个字节），
// 渲染线程随即 ioctl 读尺寸；每帧开头也再查一次兜底。尺寸变化时
// resize 纪元 + 整树补画，并经路由下发 Kind::resize 事件（首帧从
// 未知尺寸到实际尺寸也算一次）。SIGINT/SIGTERM/SIGHUP 转成正常退出路径；
// SIGKILL/SIGSEGV 无法覆盖。
//
// 能力握手（§3.3）：run() 开始时发出查询（DECRQM 2026/2027、kitty
// flags、OSC 11 背景、DA1 哨兵）并打开 L6 的应答窗口，不阻塞首帧。
// 终端按顺序应答；DA1 到达前收到的应答累积为能力升级，DA1 到达时
// 一次性提交（含推入 kitty flag 1），1 秒无 DA1 则关闭窗口保持初始值。
// 应答事件由运行时消费，不下发给应用；窗口关闭后 \e] 恢复 Alt-] 语义。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tui/document.hpp"
#include "tui/input.hpp"
#include "tui/terminal.hpp"
#include "tui/widget.hpp"

namespace dagent::tui {

using TimerId = uint64_t;

class Runtime {
public:
    struct Options {
        std::chrono::milliseconds min_frame{16}; // 合帧窗口（≈60fps 上限）
    };

    // root 是整棵控件树的根（非拥有）。run() 期间它只被渲染线程触碰。
    Runtime(Terminal& term, Widget& root, Options opt);
    Runtime(Terminal& term, Widget& root);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // ---- 配置（run() 之前，或渲染线程的应用代码内调用）----
    // 焦点有两副面孔：路由目标（L6 处理器）与光标来源（widget 的
    // cursor() 加上 screen_origin() 成屏幕坐标，帧末定位）。
    void set_focus(EventHandler* routing, Widget* cursor_source = nullptr) noexcept {
        router_.set_focus(routing);
        cursor_source_ = cursor_source;
        dirty_ = true; // 光标来源变了：即使控件都没失效也要重新定位
    }
    void set_global(EventHandler& h) noexcept {
        global_ = &h;
        router_.set_global(&h);
    }
    void push_modal(EventHandler& h) { router_.push(h); }
    void pop_modal(EventHandler& h) { router_.pop(h); }

    // ---- 鼠标命中（§3.5；run() 之前或渲染线程）----
    // 键盘路由不变；鼠标事件按坐标分发：L3/L4 不感知事件，控件与处理器的
    // 对应关系登记在这里。绑定以控件身份为准，重复绑定覆盖。控件销毁前
    // 必须 unbind_mouse（同时解除其捕获）；浮层内的控件由 close_overlay
    // 代为解除。
    void bind_mouse(Widget& w, EventHandler& h);
    void unbind_mouse(Widget& w) noexcept;

    // ---- 浮层（§3.4.3；run() 之前或渲染线程）----
    // root 必须是 LayerStack。打开并可选接管输入（modal 压入模态栈）
    // 与光标来源；point 供 above_point / at_point 使用。关闭时弹出模态、
    // 恢复打开前的光标来源。关闭顺序不是后进先出时，模态栈按 L6 的
    // 空槽语义处理，光标恢复点改接到被关浮层自己的恢复点。
    uint32_t open_overlay(std::unique_ptr<Widget> w, Placement p,
                          Point point = {},
                          EventHandler* modal = nullptr,
                          Widget* cursor_source = nullptr);
    void close_overlay(uint32_t id);

    // ---- 定时器（§3.9；run() 之前或渲染线程，回调在渲染线程执行）----
    // after：delay 后执行一次。every：每 period 执行一次，回调返回 false
    // 即取消（动画）。cancel 对已执行/已取消的 id 是空操作；回调里可以
    // 取消自己或新建定时器。
    TimerId after(std::chrono::milliseconds delay, std::function<void()> fn);
    TimerId every(std::chrono::milliseconds period, std::function<bool()> fn);
    void cancel(TimerId id);

    // 写系统剪贴板（OSC 52，§3.8；渲染线程）。渲染线程是唯一的终端写者，
    // 事件处理器与定时器回调就在渲染线程上，调用即写出，不等下一帧。
    // 返回是否完整写出（见 Terminal::set_clipboard）。
    bool set_clipboard(std::string_view text) { return term_.set_clipboard(text); }

    // ---- 更新通道（任意线程）----
    // 把 fn 交给渲染线程执行，入队即返回。fn 里改的是控件树/Document
    // （业务代码不持有它们的锁，也拿不到别的方式），禁 I/O。
    void post(std::function<void()> fn);

    // 请求退出（任意线程；只置原子标志并唤醒等待点）。
    void quit() noexcept;

    // 把调用线程变成渲染线程。阻塞直到 quit、退出类信号或 stdin 关闭。
    void run();

    // 已产出的帧数（诊断与验证用；稳态下增速即帧率）。
    uint64_t frames() const noexcept { return frames_.load(std::memory_order_relaxed); }
    // 主循环被唤醒（poll 返回）的次数（诊断与验证用；静止界面不增长）。
    uint64_t wakeups() const noexcept { return wakeups_.load(std::memory_order_relaxed); }

private:
    using Clock = std::chrono::steady_clock;

    // 以下都只在渲染线程上调用（控件树与此处状态零锁）。
    void apply_inbox();                     // 摘走更新队列并依序执行
    void route_events();                    // 路由 events_（握手应答先被运行时消费）并清空
    void check_size();                      // ioctl 尺寸；变了则纪元 + resize 事件
    void frame();                           // 尺寸 → 布局 → 光栅化 → 光标
    void note_changes() noexcept;           // 控件树失效 → 需要出帧
    int poll_timeout(Clock::time_point now) const noexcept; // -1 = 无限期
    bool on_render_thread() const noexcept;
    void wake() noexcept;
    void drain_wake() noexcept;

    // ---- 定时器（§3.9）：只在渲染线程上 ----
    void schedule(TimerId id, Clock::time_point due);
    void prune_timers();                    // 堆顶已取消的条目出堆
    void run_timers(Clock::time_point now); // 执行已到期的定时器

    // ---- 能力握手（§3.3）：只在渲染线程上 ----
    void start_handshake();                 // 发查询 + 打开应答窗口 + 起 1s 超时
    bool handle_handshake_reply(const Event& e); // 返回 true = 已消费
    void finish_handshake(bool commit);     // DA1 提交能力；超时不提交

    // ---- 鼠标命中（§3.5）：只在渲染线程上 ----
    void dispatch_mouse(const Event& e);    // 捕获 → 模态 → 命中链 → 全局
    bool deliver_mouse(EventHandler& h, const Widget* w, Event e);
    EventHandler* mouse_handler(Widget& w) const noexcept;

    Terminal& term_;
    Widget& root_;
    Options opt_;

    EventRouter router_;
    Decoder decoder_;

    // ---- 更新队列：业务线程与渲染线程之间唯一的共享状态 ----
    // 侵入式单链表而不是 vector：vector 的 push_back 只是「摊还」O(1)，
    // 一次慢帧（大文档 resize 的整树重折）期间队列能涨到十万级，那一次
    // 扩容要搬走全部已排队的 std::function —— 实测单次 post 因此突破
    // 1.5ms，违反 02 §3.1「post 单次耗时 < 1ms」。链表尾插只改两个指针，
    // 最坏情况也与队列长度无关；节点的 new/delete 都在锁外。
    struct Task {
        std::function<void()> fn;
        Task* next;
    };
    std::mutex queue_mutex_;     // 只保护 inbox_head_ / inbox_tail_
    Task* inbox_head_ = nullptr; // 业务线程尾插，渲染线程整条摘走
    Task* inbox_tail_ = nullptr;

    // ---- 仅渲染线程触碰（控件树与下列状态，零锁）----
    bool dirty_ = true;    // 需要出帧（首帧）
    std::optional<Clock::time_point> esc_due_; // Esc 歧义超时的截止时刻
    Widget* cursor_source_ = nullptr;
    std::vector<Event> events_; // 复用：解码事件的缓冲
    Size size_{};               // 已知终端尺寸（{0,0} = 未知）

    // 握手（§3.3）：窗口期间累积 pending_caps_，DA1 到达才提交。
    bool handshake_active_ = false;
    Terminal::Caps pending_caps_{};
    std::optional<Clock::time_point> reply_due_; // 1 秒未收到 DA1 的截止时刻

    // 定时器（§3.9）：登记表 + 到期时刻最小堆（惰性删除）。
    struct TimerEntry {
        std::function<void()> once;   // after
        std::function<bool()> repeat; // every
        std::chrono::milliseconds period{0}; // 0 = after
    };
    struct TimerSlot {
        Clock::time_point due;
        TimerId id;
    };
    struct TimerLater { // 最小堆：到期早的在顶，同时到期的先建先出
        bool operator()(const TimerSlot& a, const TimerSlot& b) const noexcept {
            return a.due != b.due ? a.due > b.due : a.id > b.id;
        }
    };
    std::unordered_map<TimerId, TimerEntry> timers_;
    std::vector<TimerSlot> timer_heap_;
    std::vector<TimerSlot> fired_; // 复用：本轮到期的条目
    TimerId next_timer_ = 1;

    // 浮层（§3.4）：记录每个打开浮层的模态处理器与光标恢复点。
    // stack_ 在构造时取得；root 不是 LayerStack 时浮层 API 不可用。
    struct OverlayState {
        uint32_t id = 0;
        Widget* widget = nullptr; // 浮层矩形（模态鼠标判定的边界）
        EventHandler* modal = nullptr;
        Widget* cursor_source = nullptr;
        Widget* prev_cursor = nullptr;
    };
    LayerStack* stack_ = nullptr;
    std::vector<OverlayState> overlays_;

    // 鼠标（§3.5）：控件 → 处理器登记表；捕获记录按下时消费的处理器，
    // 直到释放前移动/释放都直接交给它。
    struct MouseCapture {
        Widget* widget = nullptr; // 相对坐标的参照；全局处理器为 nullptr
        EventHandler* handler = nullptr;
        int button = -1;
    };
    std::vector<std::pair<Widget*, EventHandler*>> mouse_bindings_;
    MouseCapture capture_;
    EventHandler* global_ = nullptr;

    // ---- 跨线程原子 ----
    std::atomic<bool> quit_{false};
    std::atomic<bool> wake_pending_{false}; // 管道里已有未消费的唤醒字节
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> wakeups_{0};
    std::atomic<std::thread::id> render_thread_{};

    // ---- 仅渲染线程触碰（帧缓冲与输出）----
    Clock::time_point last_frame_{};
    Surface back_;
    Surface front_;
    std::string out_; // 复用容量：稳态帧路径零分配
    std::optional<Point> cursor_;
    int wake_pipe_[2] = {-1, -1}; // post()/quit() 唤醒 poll 的管道
};

// 滚动区的鼠标翻译（§3.8）：rt.bind_mouse(scrollback, handler)。
//   * 左键拖拽选择（依赖 §3.5 的捕获：拖出控件也归它）；单击不拖拽清除选区；
//   * 双击选词、三击选逻辑行（400ms 内同一格连击，由 §3.9 定时器判定）；
//   * 释放时把选区源文本写入剪贴板（OSC 52），copy_on_release = false 关闭；
//   * 滚轮滚动 3 行。
// 需要应用开启鼠标上报（Terminal::set_mouse(true)，1002 模式才上报拖拽）。
// 生命周期：必须先于 Runtime 与 Scrollback 销毁（析构取消连击定时器）。
class ScrollbackMouse : public EventHandler {
public:
    static constexpr std::chrono::milliseconds k_multi_click{400};

    ScrollbackMouse(Runtime& rt, Scrollback& sb) noexcept : rt_(rt), sb_(sb) {}
    ~ScrollbackMouse() override;
    ScrollbackMouse(const ScrollbackMouse&) = delete;
    ScrollbackMouse& operator=(const ScrollbackMouse&) = delete;

    bool copy_on_release = true;

    bool on_event(const Event& e) override;

private:
    Runtime& rt_;
    Scrollback& sb_;
    TimerId click_timer_ = 0; // 连击窗口（到期即清零 clicks_）
    int clicks_ = 0;
    Point last_press_{-1, -1};
    Location press_{};        // 拖拽起点
    bool dragging_ = false;   // 单击按下后，拖动即选择
};

} // namespace dagent::tui
