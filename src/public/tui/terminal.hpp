// L1 终端抽象：进入/退出界面模式、能力探测、尺寸报告、字节写出口。
// 不做任何绘制决策；全框架只有渲染线程会调用 write()。
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>
#include <termios.h>

namespace dagent::tui {

// 视口尺寸（列 × 行）。布局纪元与 Surface 尺寸都由它驱动。
struct Size {
    int cols = 0;
    int rows = 0;
    bool operator==(const Size&) const noexcept = default;
};

// 颜色：1 字节 tag + 3 字节值，恰好 4 字节；indexed 时 r 即调色板索引。
// 定义在 L1 是因为 Terminal::Caps 需要携带握手取得的背景色（§3.3）。
struct Color {
    enum class Kind : uint8_t { default_, indexed, rgb };
    Kind kind = Kind::default_;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    bool operator==(const Color&) const noexcept = default;

    static constexpr Color indexed(uint8_t i) noexcept {
        return {Kind::indexed, i, 0, 0};
    }
    static constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) noexcept {
        return {Kind::rgb, r, g, b};
    }
};

// 终端会话。
//
// RAII：构造即进入界面模式（备用屏 + 关自动换行 + 藏光标 + raw termios），
// 析构逆序还原。还原有三条保障路径：
//   1. 析构（正常返回 / 栈展开）；
//   2. SIGINT/SIGTERM/SIGHUP → self-pipe 唤醒主循环，走正常退出路径；
//   3. atexit 兜底（restore() 幂等，多路径可安全叠加）。
// SIGKILL/SIGSEGV 无法覆盖，是终端程序的共同边界。
//
// 挂起/恢复（§3.10）与还原共用同一套转义栈，但不是终态：suspend() 逆序
// 退出界面模式并还原 termios（信号处理器保留），resume() 重新进入并恢复
// 挂起前开着的模式；restore() 之后 suspend()/resume() 均为空操作。
class Terminal {
public:
    // 终端能力。进入时按环境变量取初始值，Runtime 握手（§3.3）确认后
    // 由渲染线程升级；不支持的做合理降级。
    struct Caps {
        bool truecolor       = false; // COLORTERM=truecolor / 24bit
        bool synchronized    = false; // DEC 2026 同步输出（逐帧包裹用）
        bool sgr_mouse       = false; // 1006 扩展鼠标上报（>223 列必需）
        bool bracketed_paste = false; // 2004 括号粘贴
        bool focus_events    = false; // 1004 焦点事件
        bool kitty_keyboard  = false; // kitty 键盘协议（确认后推入 flag 1）
        bool grapheme_width  = false; // mode 2027 字素簇宽度（§3.13）
        std::optional<Color> background; // OSC 11 背景色；未取得时为空
    };

    Terminal();
    ~Terminal();

    Terminal(const Terminal&)            = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&&)                 = delete;
    Terminal& operator=(Terminal&&)      = delete;

    const Caps& caps() const noexcept { return caps_; }

    // 握手应答结果（§3.3，渲染线程调用）：以应答为准写入能力记录。
    // 调用方传入「初始值 + 已收到的应答覆盖」后的副本，未应答的查询
    // 保持环境变量初始值（包括显式应答“不支持”时覆盖猜测的降级）。
    void apply_caps(const Caps& caps) noexcept;

    // 剪贴板（§3.8）：写 OSC 52 \e]52;c;<base64>\a，由渲染线程调用。
    // 上限 1 MiB 原文，超出按 UTF-8 边界截断。返回是否完整写出（false =
    // 已截断或未进入界面模式），截断提示归应用层。tmux 需要
    // set-clipboard on，属使用方配置。
    bool set_clipboard(std::string_view text);

    // kitty 键盘协议（§3.2.2）：握手确认支持后由渲染线程推入 flag 1，
    // restore 逆序弹出 \e[<u。能力缺失或未进入界面模式时空操作。
    void set_kitty_keyboard(bool on);

    // 能否发出终端查询（§3.3 握手）：stdout 已进入界面模式、stdin 是
    // raw 模式的 tty，两者缺一不可 —— 否则查询会写进管道，或应答落进
    // 无人读取的 tty 输入队列，程序退出后作为杂字出现在 shell 里。
    bool can_query() const noexcept {
        return raw_saved_ && screen_active_.load(std::memory_order_acquire);
    }

    // 渲染线程查询（ioctl TIOCGWINSZ 约 1µs），尺寸变化时由上层提升布局纪元。
    // 尺寸只在这里读；SIGWINCH 处理器只往 self-pipe 写一个字节唤醒空闲的
    // 渲染线程，不在信号上下文里做任何别的事。非 tty 时回退 80×24。
    Size size() const noexcept;

    // 唯一输出出口。由渲染线程调用；渲染前已组装完整差分帧。
    // 阻塞式写满全部字节（EINTR 重试），不做用户态缓冲。
    void write(std::string_view bytes) noexcept;

    // 可选上报模式的开关。能力缺失或未进入界面模式时是空操作。
    // 鼠标上报（1002 + 1006 SGR：按下/释放/拖拽/滚轮）开启后终端不再做
    // 原生选择（多数终端按住 Shift 仍可原生选择），应用内选择由 §3.8 的
    // ScrollbackMouse 接管；是否开启由应用决定，故默认关闭。
    // 括号粘贴由构造按能力开启，不提供运行时开关。
    void set_mouse(bool on);
    void set_focus_events(bool on);

    // 还原进入前的终端状态（转义逆序 + termios）。幂等，可重复调用，
    // 也是 atexit 兜底路径的入口。
    void restore() noexcept;

    // 挂起/恢复（§3.10，渲染线程执行，与 restore 共用逆序栈）：
    // suspend() 逆序退出界面模式、还原 termios 并记住挂起前的鼠标/焦点/
    // 粘贴/kitty 模式；resume() 逆序重新进入（含备用屏与 raw）。挂起期间
    // SIGINT 不触发退出（Ctrl+C 属于占用前台的外部程序），SIGTERM/SIGHUP
    // 照常。两者配对
    // 使用，suspend 后 restore 仍是终态、resume 变空操作。stdout/stdin
    // 非 tty 时退化为空操作。
    void suspend() noexcept;
    void resume();

    // self-pipe 读端：信号处理器只写一个字节唤醒，主循环 poll 此 fd。
    // 接管 SIGINT/SIGTERM/SIGHUP（退出类）与 SIGWINCH（尺寸变化）。
    // O_NONBLOCK | O_CLOEXEC。
    int signal_fd() const noexcept { return signal_pipe_[0]; }

    // 自上次清空以来收到过的信号类别。
    struct Signals {
        bool quit   = false; // SIGINT / SIGTERM / SIGHUP：走正常退出路径
        bool resize = false; // SIGWINCH：重新查询 size()
    };

    // 主循环被唤醒后清空管道中的信号字节，报告收到了哪几类信号。
    Signals drain_signal() noexcept;

private:
    void enter();
    void probe_caps() noexcept;
    void install_signal_handlers() noexcept;
    void uninstall_signal_handlers() noexcept;

    Caps caps_;

    termios saved_{};
    bool raw_saved_ = false;        // 仅当 stdin 是 tty 且 tcgetattr 成功时为 true
    std::atomic<bool> screen_active_{false}; // 仅当 stdout 是 tty 且已发出进入序列时为 true；
                                            // 与 mouse_/focus_/paste_ 同为跨线程（atexit 还原路径）访问
    bool handlers_installed_ = false;

    int signal_pipe_[2] = {-1, -1};

    std::atomic<bool> restored_{false}; // restore() 只执行一次的保证
    std::atomic<bool> mouse_{false};
    std::atomic<bool> focus_{false};
    std::atomic<bool> paste_{false};
    std::atomic<bool> kitty_{false};

    // 挂起现场（只由 suspend/resume 在渲染线程读写；restore 不碰）。
    bool suspended_ = false;
    bool suspended_screen_ = false; // 挂起前在界面模式
    bool suspended_raw_ = false;    // 挂起前 stdin 是 raw
    bool suspended_mouse_ = false;
    bool suspended_focus_ = false;
    bool suspended_paste_ = false;
    bool suspended_kitty_ = false;
};

} // namespace dagent::tui
