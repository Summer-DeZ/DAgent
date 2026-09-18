/// @file terminal.hpp
/// @brief 终端抽象：进入/退出界面模式、能力探测、尺寸查询、字节写出。
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>
#include <termios.h>

namespace dagent::tui {

/// @brief 视口尺寸（列 × 行）。
struct Size {
    int cols = 0;
    int rows = 0;
    bool operator==(const Size&) const noexcept = default;
};

/// @brief 颜色：kind 决定 r/g/b 的含义（indexed 时 r 是调色板索引）。
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

/// @brief 终端会话（RAII：构造进入界面模式，析构还原）。
class Terminal {
public:
    /// @brief 终端能力。
    struct Caps {
        bool truecolor       = false; ///< COLORTERM=truecolor / 24bit
        bool synchronized    = false; ///< DEC 2026 同步输出
        bool sgr_mouse       = false; ///< 1006 扩展鼠标上报
        bool bracketed_paste = false; ///< 2004 括号粘贴
        bool focus_events    = false; ///< 1004 焦点事件
        bool kitty_keyboard  = false; ///< kitty 键盘协议
        bool grapheme_width  = false; ///< mode 2027 字素簇宽度
        std::optional<Color> background; ///< OSC 11 背景色；未取得时为空
    };

    Terminal();
    ~Terminal();

    Terminal(const Terminal&)            = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&&)                 = delete;
    Terminal& operator=(Terminal&&)      = delete;

    const Caps& caps() const noexcept { return caps_; }

    /// @brief 写入握手确认后的能力（未应答项保持原值）。
    void apply_caps(const Caps& caps) noexcept;

    /// @brief 写 OSC 52 剪贴板；超 1 MiB 按 UTF-8 边界截断，返回是否完整写出。
    bool set_clipboard(std::string_view text);

    /// @brief 开关 kitty 键盘协议；不支持时为空操作。
    void set_kitty_keyboard(bool on);

    /// @brief 开关字素簇宽度模式（mode 2027）；不支持时为空操作。
    void set_grapheme_width(bool on);

    /// @brief 终端查询是否可用（需要 stdout 在界面模式且 stdin 是 raw tty）。
    bool can_query() const noexcept {
        return raw_saved_ && screen_active_.load(std::memory_order_acquire);
    }

    /// @brief 读取终端尺寸；非 tty 时返回 80×24。
    Size size() const noexcept;

    /// @brief 写出字节，阻塞写满。
    void write(std::string_view bytes) noexcept;

    /// @brief 开关鼠标上报（1002 + 1006 SGR）。
    void set_mouse(bool on);
    /// @brief 开关焦点事件上报（1004）。
    void set_focus_events(bool on);

    /// @brief 还原进入前的终端状态；幂等。
    void restore() noexcept;

    /// @brief 挂起界面模式并记住当前开关，等待 resume()。
    void suspend() noexcept;
    /// @brief 恢复 suspend() 前的界面模式。
    void resume();

    /// @brief 信号 self-pipe 读端（poll 用）。
    int signal_fd() const noexcept { return signal_pipe_[0]; }

    /// @brief 已收到的信号类别。
    struct Signals {
        bool quit   = false; ///< SIGINT / SIGTERM / SIGHUP
        bool resize = false; ///< SIGWINCH
    };

    /// @brief 读走管道中的信号字节并返回类别。
    Signals drain_signal() noexcept;

private:
    void enter();
    void probe_caps() noexcept;
    void install_signal_handlers() noexcept;
    void uninstall_signal_handlers() noexcept;

    Caps caps_;

    termios saved_{};
    bool raw_saved_ = false; ///< stdin 是 tty 且已保存 termios
    std::atomic<bool> screen_active_{false}; ///< stdout 已进入界面模式
    bool handlers_installed_ = false;

    int signal_pipe_[2] = {-1, -1};

    std::atomic<bool> restored_{false}; ///< restore() 已执行
    std::atomic<bool> mouse_{false};
    std::atomic<bool> focus_{false};
    std::atomic<bool> paste_{false};
    std::atomic<bool> kitty_{false};
    std::atomic<bool> grapheme_{false};

    // ---- 挂起现场 ----
    bool suspended_ = false;
    bool suspended_screen_ = false; ///< 挂起前在界面模式
    bool suspended_raw_ = false;    ///< 挂起前 stdin 是 raw
    bool suspended_mouse_ = false;
    bool suspended_focus_ = false;
    bool suspended_paste_ = false;
    bool suspended_kitty_ = false;
    bool suspended_grapheme_ = false;
};

} // namespace dagent::tui
