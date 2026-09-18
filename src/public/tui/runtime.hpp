/// @file runtime.hpp
/// @brief 渲染运行时：控件树归渲染线程独占，业务线程经 post() 入队更新。
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

/// @brief 运行时：调度出帧、输入路由、定时器、更新队列与终端能力握手。
class Runtime {
public:
    struct Options {
        std::chrono::milliseconds min_frame{16}; ///< 合帧窗口
    };

    /// @brief root 是控件树根（非拥有）。
    Runtime(Terminal& term, Widget& root, Options opt);
    Runtime(Terminal& term, Widget& root);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // ---- 配置（run() 之前或渲染线程）----

    /// @brief 设置焦点处理器与光标来源控件。
    void set_focus(EventHandler* routing, Widget* cursor_source = nullptr) noexcept {
        router_.set_focus(routing);
        cursor_source_ = cursor_source;
        dirty_ = true; // 光标来源变化也要重新定位
    }
    /// @brief 设置全局兜底处理器。
    void set_global(EventHandler& h) noexcept {
        global_ = &h;
        router_.set_global(&h);
    }
    /// @brief 压入/弹出模态处理器。
    void push_modal(EventHandler& h) { router_.push(h); }
    void pop_modal(EventHandler& h) { router_.pop(h); }

    // ---- 鼠标（run() 之前或渲染线程）----

    /// @brief 登记控件的鼠标处理器（重复绑定覆盖）。
    /// @note 控件销毁前必须 unbind_mouse。
    void bind_mouse(Widget& w, EventHandler& h);
    void unbind_mouse(Widget& w) noexcept;

    // ---- 浮层（run() 之前或渲染线程）----

    /// @brief 打开浮层；modal 压入模态栈，cursor_source 接管光标。返回浮层 id。
    /// @note root 必须是 LayerStack。
    uint32_t open_overlay(std::unique_ptr<Widget> w, Placement p,
                          Point point = {},
                          EventHandler* modal = nullptr,
                          Widget* cursor_source = nullptr);
    /// @brief 关闭浮层并恢复其接过的一切。
    void close_overlay(uint32_t id);

    /// @brief 终端能力确定时回调（每次 run() 至多一次，已确定时立即回调）。
    void on_caps(std::function<void(const Terminal::Caps&)> fn);

    // ---- 定时器（run() 之前或渲染线程）----

    /// @brief delay 后执行一次。
    TimerId after(std::chrono::milliseconds delay, std::function<void()> fn);
    /// @brief 每 period 执行一次，回调返回 false 即取消。
    TimerId every(std::chrono::milliseconds period, std::function<bool()> fn);
    /// @brief 取消定时器；对已执行/已取消的 id 是空操作。
    void cancel(TimerId id);

    // ---- 挂起/恢复（渲染线程内调用）----

    /// @brief 挂起界面执行 fn（如外部编辑器），完成后恢复并整屏重画。
    void run_external(std::function<void()> fn);
    /// @brief 挂起框架并向进程组发 SIGTSTP（Ctrl+Z），SIGCONT 后恢复并重画。
    void suspend_process();

    /// @brief 写系统剪贴板；返回是否完整写出。
    bool set_clipboard(std::string_view text) { return term_.set_clipboard(text); }

    // ---- 更新通道（任意线程）----

    /// @brief 把 fn 交给渲染线程执行，入队即返回；fn 里禁 I/O。
    void post(std::function<void()> fn);

    /// @brief 请求退出。
    void quit() noexcept;

    /// @brief 进入渲染循环（当前线程即渲染线程），阻塞至退出。
    void run();

    /// @brief 累计帧数。
    uint64_t frames() const noexcept { return frames_.load(std::memory_order_relaxed); }
    /// @brief 主循环唤醒次数。
    uint64_t wakeups() const noexcept { return wakeups_.load(std::memory_order_relaxed); }

private:
    using Clock = std::chrono::steady_clock;

    // ---- 仅渲染线程调用 ----
    void apply_inbox();                     ///< 摘取并执行更新队列
    void route_events();                    ///< 路由事件（握手应答先被消费）
    void check_size();                      ///< 探测尺寸，变化则升起 resize 纪元
    void frame();                           ///< 布局 + 渲染到 back_
    void note_changes() noexcept;
    int poll_timeout(Clock::time_point now) const noexcept; ///< -1 表示无限期
    bool on_render_thread() const noexcept;
    void wake() noexcept;
    void drain_wake() noexcept;

    void schedule(TimerId id, Clock::time_point due);
    void prune_timers();                    ///< 丢弃堆顶已取消的条目
    void run_timers(Clock::time_point now);

    void start_handshake();
    bool handle_handshake_reply(const Event& e); ///< 返回 true = 已消费
    void finish_handshake(bool commit);     ///< commit=false 表示超时
    void report_caps();

    void resume_after_suspend();

    void dispatch_mouse(const Event& e);
    bool deliver_mouse(EventHandler& h, const Widget* w, Event e);
    EventHandler* mouse_handler(Widget& w) const noexcept;

    Terminal& term_;
    Widget& root_;
    Options opt_;

    EventRouter router_;
    Decoder decoder_;

    // ---- 更新队列：业务线程与渲染线程间唯一的共享状态 ----
    struct Task {
        std::function<void()> fn;
        Task* next;
    };
    std::mutex queue_mutex_;     ///< 保护 inbox_head_ / inbox_tail_
    Task* inbox_head_ = nullptr; ///< 业务线程尾插，渲染线程整条摘走
    Task* inbox_tail_ = nullptr;

    // ---- 仅渲染线程触碰 ----
    bool dirty_ = true;    ///< 需要出帧
    std::optional<Clock::time_point> esc_due_; ///< Esc 歧义超时截止时刻
    Widget* cursor_source_ = nullptr;
    std::vector<Event> events_; ///< 解码事件缓冲
    Size size_{};               ///< {0,0} = 尺寸未知

    bool handshake_active_ = false;
    bool caps_final_ = false; ///< 本次 run() 已回调过 on_caps
    std::function<void(const Terminal::Caps&)> caps_fn_;
    Terminal::Caps pending_caps_{}; ///< 握手期间累积，DA1 到达才提交
    std::optional<Clock::time_point> reply_due_; ///< 等待 DA1 的截止时刻

    struct TimerEntry {
        std::function<void()> once;   ///< after
        std::function<bool()> repeat; ///< every
        std::chrono::milliseconds period{0}; ///< 0 = after
    };
    struct TimerSlot {
        Clock::time_point due;
        TimerId id;
    };
    struct TimerLater {
        bool operator()(const TimerSlot& a, const TimerSlot& b) const noexcept {
            return a.due != b.due ? a.due > b.due : a.id > b.id;
        }
    };
    std::unordered_map<TimerId, TimerEntry> timers_;
    std::vector<TimerSlot> timer_heap_;
    std::vector<TimerSlot> fired_; ///< 本轮到期条目
    TimerId next_timer_ = 1;

    struct OverlayState {
        uint32_t id = 0;
        Widget* widget = nullptr; ///< 模态鼠标判定的边界
        EventHandler* modal = nullptr;
        Widget* cursor_source = nullptr;
        Widget* prev_cursor = nullptr; ///< 接管前的光标来源
    };
    LayerStack* stack_ = nullptr;
    std::vector<OverlayState> overlays_;

    struct MouseCapture {
        Widget* widget = nullptr; ///< 相对坐标参照；全局处理器为 nullptr
        EventHandler* handler = nullptr;
        int button = -1;
    };
    std::vector<std::pair<Widget*, EventHandler*>> mouse_bindings_;
    MouseCapture capture_; ///< 按下后移动/释放都交给它
    EventHandler* global_ = nullptr;

    // ---- 跨线程原子 ----
    std::atomic<bool> quit_{false};
    std::atomic<bool> wake_pending_{false}; ///< 管道内已有未消费的唤醒字节
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> wakeups_{0};
    std::atomic<std::thread::id> render_thread_{};

    // ---- 仅渲染线程触碰 ----
    Clock::time_point last_frame_{};
    Surface back_;
    Surface front_;
    std::string out_; ///< 输出缓冲（复用容量）
    std::optional<Point> cursor_;
    int wake_pipe_[2] = {-1, -1}; ///< 唤醒 poll 的管道
};

/// @brief 滚动区鼠标处理：左键拖拽选择、双击选词、三击选行、滚轮滚动；
/// 释放时把选区写入剪贴板（copy_on_release 控制）。
/// @note 必须先于 Runtime 与 Scrollback 销毁。
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
    TimerId click_timer_ = 0; ///< 连击窗口计时器
    int clicks_ = 0;
    Point last_press_{-1, -1};
    Location press_{};      ///< 拖拽起点
    bool dragging_ = false;
};

/// @brief 命令表项：id 供绑定引用，title/category 供命令面板显示。
struct Command {
    std::string id;       ///< 如 "session.new"
    std::string title;
    std::string category;
    std::function<void()> run;
    std::function<bool()> enabled; ///< 空 = 始终可用
};

/// @brief 快捷键与命令层：绑定串（空格分隔，如 "ctrl+p"、"<leader> n"）到命令。
/// @note 必须先于 Runtime 销毁。
class Keymap : public EventHandler {
public:
    explicit Keymap(Runtime& rt) noexcept;
    ~Keymap() override;
    Keymap(const Keymap&) = delete;
    Keymap& operator=(const Keymap&) = delete;

    /// @brief 添加命令；重复 id 覆盖。
    void add(Command c);
    /// @brief 注册绑定；按键串非法或命令 id 不存在时返回 false。
    bool bind(std::string_view keys, std::string_view command_id);
    /// @brief 设置 <leader> 按键与序列等待超时；按键串非法则清除 leader。
    void set_leader(std::string_view key, std::chrono::milliseconds timeout);
    const std::vector<Command>& commands() const noexcept { return commands_; }

    bool on_event(const Event& e) override;

private:
    struct KeyPress {
        Key key = Key::none;
        char32_t ch = 0; ///< 可打印码点；与 key 互斥
        Mods mods = Mods::none;
        bool operator==(const KeyPress&) const noexcept = default;
    };
    struct Token {
        bool leader = false; ///< "<leader>" 占位
        KeyPress key{};
        bool operator==(const Token&) const noexcept = default;
    };
    struct Binding {
        std::vector<Token> keys;
        size_t command = 0; ///< commands_ 下标
    };

    static std::optional<KeyPress> press_from(const Event& e);
    static bool parse_key(std::string_view text, Mods mods, KeyPress& out);
    static bool parse_token(std::string_view text, Token& out);
    static bool parse_binding(std::string_view text, std::vector<Token>& out);

    void arm_timeout();
    void reset_sequence();
    void execute(size_t command_index);

    Runtime& rt_;
    std::vector<Command> commands_;
    std::vector<Binding> bindings_;
    std::optional<KeyPress> leader_;
    std::chrono::milliseconds timeout_{500}; ///< 序列等待超时
    std::vector<KeyPress> pending_; ///< 已按下的序列前缀（非空 = 模态已压栈）
    bool modal_active_ = false;
    TimerId timeout_id_ = 0;
};

} // namespace dagent::tui
