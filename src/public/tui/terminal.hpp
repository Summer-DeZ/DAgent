// L1 终端抽象：进入/退出界面模式、能力探测、尺寸报告、字节写出口。
// 不做任何绘制决策；全框架只有渲染线程会调用 write()。
#pragma once

#include <atomic>
#include <string_view>
#include <termios.h>

namespace dagent::tui {

// 视口尺寸（列 × 行）。布局纪元与 Surface 尺寸都由它驱动。
struct Size {
    int cols = 0;
    int rows = 0;
    bool operator==(const Size&) const noexcept = default;
};

// 终端会话。
//
// RAII：构造即进入界面模式（备用屏 + 关自动换行 + 藏光标 + raw termios），
// 析构逆序还原。还原有三条保障路径：
//   1. 析构（正常返回 / 栈展开）；
//   2. SIGINT/SIGTERM/SIGHUP → self-pipe 唤醒主循环，走正常退出路径；
//   3. atexit 兜底（restore() 幂等，多路径可安全叠加）。
// SIGKILL/SIGSEGV 无法覆盖，是终端程序的共同边界。
class Terminal {
public:
    // 终端能力。进入时按能力决定开启哪些模式，不支持的做合理降级。
    struct Caps {
        bool truecolor       = false; // COLORTERM=truecolor / 24bit
        bool synchronized    = false; // DEC 2026 同步输出（逐帧包裹用）
        bool sgr_mouse       = false; // 1006 扩展鼠标上报（>223 列必需）
        bool bracketed_paste = false; // 2004 括号粘贴
        bool focus_events    = false; // 1004 焦点事件
    };

    Terminal();
    ~Terminal();

    Terminal(const Terminal&)            = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&&)                 = delete;
    Terminal& operator=(Terminal&&)      = delete;

    const Caps& caps() const noexcept { return caps_; }

    // 每帧查询一次（ioctl TIOCGWINSZ 约 1µs），尺寸变化时由上层提升布局纪元。
    // 不用 SIGWINCH：避免异步信号上下文的复杂度。非 tty 时回退 80×24。
    Size size() const noexcept;

    // 唯一输出出口。由渲染线程调用；渲染前已组装完整差分帧。
    // 阻塞式写满全部字节（EINTR 重试），不做用户态缓冲。
    void write(std::string_view bytes) noexcept;

    // 可选上报模式的开关。能力缺失或未进入界面模式时是空操作；
    // 鼠标开启会牺牲文本选择（多数终端需按住 Shift），故默认关闭。
    // 括号粘贴由构造按能力开启，不提供运行时开关。
    void set_mouse(bool on);
    void set_focus_events(bool on);

    // 还原进入前的终端状态（转义逆序 + termios）。幂等，可重复调用，
    // 也是 atexit 兜底路径的入口。
    void restore() noexcept;

    // self-pipe 读端：信号处理器只写一个字节唤醒，主循环 poll 此 fd。
    // O_NONBLOCK | O_CLOEXEC。
    int signal_fd() const noexcept { return signal_pipe_[0]; }

    // 主循环被唤醒后清空管道中的信号字节。
    void drain_signal() noexcept;

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
};

} // namespace dagent::tui
