#include "tui/terminal.hpp"
#include "base/platform.hpp"

#include <string>

namespace dagent::tui {

namespace {

// 离开界面模式的序列；传入"当前确实开着"的模式。
std::string leave_screen_seq(bool mouse, bool paste, bool kitty,
                             bool grapheme) {
    std::string seq;
    if (mouse) seq += "\x1b[?1006l\x1b[?1002l";
    if (kitty) seq += "\x1b[<u"; // 弹出握手时推入的键盘 flag
    if (grapheme) seq += "\x1b[?2027l";
    if (paste) seq += "\x1b[?2004l";
    seq += "\x1b[?25h";   // 显示光标
    seq += "\x1b[?7h";    // 恢复自动换行
    seq += "\x1b[?1049l"; // 离开备用屏，还原用户原有终端内容
    return seq;
}

} // namespace

// 进入界面模式：顺序即栈序，还原时严格逆序弹出；stdout 非 tty 时不发任何序列。
void Terminal::enter() {
    if (!base::stdout_is_terminal()) {
        return;
    }

    std::string seq;
    seq.reserve(64);
    seq += "\x1b[?1049h"; // 备用屏幕：退出后完整还原用户原有内容
    seq += "\x1b[?7l";    // 关自动换行（DECAWM）：折行由框架控制，否则网格坐标全乱
    seq += "\x1b[?25l";   // 藏光标：帧末由渲染器定位后再显示
    if (caps_.bracketed_paste) {
        seq += "\x1b[?2004h"; // 粘贴内容被 \e[200~…\e[201~ 包裹，可区分逐字输入
        paste_.store(true, std::memory_order_release);
    }
    write(seq);
    screen_active_.store(true, std::memory_order_release);
}

// 开关鼠标上报；exchange 去重。
void Terminal::set_mouse(bool on) {
    if (!screen_active_.load(std::memory_order_acquire) || !caps_.sgr_mouse ||
        mouse_.exchange(on, std::memory_order_acq_rel) == on) {
        return;
    }
    // 1002 = 按键事件跟踪；1006 = SGR 扩展坐标。
    write(on ? "\x1b[?1002h\x1b[?1006h" : "\x1b[?1006l\x1b[?1002l");
}

void Terminal::apply_caps(const Caps& caps) noexcept { caps_ = caps; }

bool Terminal::set_clipboard(std::string_view text) {
    if (!screen_active_.load(std::memory_order_acquire)) return false;
    constexpr size_t k_max = size_t{1} << 20;
    bool complete = true;
    if (text.size() > k_max) {
        size_t cut = k_max;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut; // 不从多字节字符中间截断
        }
        text = text.substr(0, cut);
        complete = false;
    }
    static constexpr char k_b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string seq = "\x1b]52;c;";
    seq.reserve(seq.size() + (text.size() + 2) / 3 * 4 + 1);
    size_t i = 0;
    for (; i + 3 <= text.size(); i += 3) {
        const uint32_t v = (static_cast<uint8_t>(text[i]) << 16) |
                           (static_cast<uint8_t>(text[i + 1]) << 8) |
                           static_cast<uint8_t>(text[i + 2]);
        seq += k_b64[(v >> 18) & 63];
        seq += k_b64[(v >> 12) & 63];
        seq += k_b64[(v >> 6) & 63];
        seq += k_b64[v & 63];
    }
    if (i < text.size()) {
        const bool two = i + 1 < text.size();
        const uint32_t v = (static_cast<uint8_t>(text[i]) << 16) |
                           (two ? static_cast<uint8_t>(text[i + 1]) << 8 : 0);
        seq += k_b64[(v >> 18) & 63];
        seq += k_b64[(v >> 12) & 63];
        seq += two ? k_b64[(v >> 6) & 63] : '=';
        seq += '=';
    }
    seq += '\a';
    write(seq);
    return complete;
}

void Terminal::set_kitty_keyboard(bool on) {
    if (!screen_active_.load(std::memory_order_acquire) ||
        !caps_.kitty_keyboard ||
        kitty_.exchange(on, std::memory_order_acq_rel) == on) {
        return;
    }
    write(on ? "\x1b[>1u" : "\x1b[<u");
}

void Terminal::set_grapheme_width(bool on) {
    if (!screen_active_.load(std::memory_order_acquire) ||
        !caps_.grapheme_width ||
        grapheme_.exchange(on, std::memory_order_acq_rel) == on) {
        return;
    }
    write(on ? "\x1b[?2027h" : "\x1b[?2027l");
}

// 还原为进入序列的严格逆序，只关自己开过的模式。
void Terminal::restore() noexcept {
    if (restored_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (screen_active_.load(std::memory_order_acquire)) {
        write(leave_screen_seq(
            mouse_.exchange(false, std::memory_order_acq_rel),
            paste_.exchange(false, std::memory_order_acq_rel),
            kitty_.exchange(false, std::memory_order_acq_rel),
            grapheme_.exchange(false, std::memory_order_acq_rel)));
        screen_active_.store(false, std::memory_order_release);
    }

    restore_input();
}

} // namespace dagent::tui
