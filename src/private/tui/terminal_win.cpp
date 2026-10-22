// Windows 控制台版：输入走 ReadConsoleInputW，输出走 VT 序列。
#include "tui/event_wait.hpp"
#include "tui/terminal.hpp"
#include "base/platform.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>

#include <windows.h>

namespace dagent::tui {

namespace {

// atexit 兜底与控制台事件处理器（无 this 可用）所需的全局锚点。
std::atomic<Terminal*> g_instance{nullptr};
std::once_flag g_atexit_once;

// 手动重置事件加两个待处理标志，对应 POSIX 的 self-pipe。
struct SignalState {
    std::atomic<HANDLE> event{nullptr};
    std::atomic<bool> quit_pending{false};
    std::atomic<bool> resize_pending{false};
};
SignalState g_signal;

// atexit 兜底：进程退出前还原。
void terminal_atexit_restore() noexcept {
    if (Terminal* t = g_instance.load(std::memory_order_acquire)) {
        t->restore();
    }
}

// 关闭/注销/关机：置退出并立即还原（进程随后被结束）。
// Ctrl-C/Ctrl-Break 不接管：raw 模式下 Ctrl-C 是键输入，Ctrl-Break 交给进程级处理。
BOOL WINAPI terminal_console_control(DWORD type) {
    switch (type) {
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_signal.quit_pending.store(true, std::memory_order_release);
        if (const HANDLE event = g_signal.event.load(std::memory_order_acquire)) {
            ::SetEvent(event);
        }
        if (Terminal* t = g_instance.load(std::memory_order_acquire)) {
            t->restore();
        }
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

struct Terminal::Native {
    HANDLE input        = INVALID_HANDLE_VALUE;
    HANDLE output       = INVALID_HANDLE_VALUE;
    HANDLE signal_event = nullptr;
    DWORD saved_input_mode  = 0;
    DWORD saved_output_mode = 0;
    UINT saved_output_cp    = 0;
    bool input_saved  = false;
    bool output_saved = false;
    bool cp_saved     = false;
    bool raw_saved_   = false;
    bool handlers_installed_ = false;
};

Terminal::Terminal() {
    probe_caps();
    native_ = std::make_unique<Native>();
    native_->input  = ::GetStdHandle(STD_INPUT_HANDLE);
    native_->output = ::GetStdHandle(STD_OUTPUT_HANDLE);

    // 事件创建失败只失去信号唤醒能力，不影响其余功能。
    g_signal.quit_pending.store(false, std::memory_order_relaxed);
    g_signal.resize_pending.store(false, std::memory_order_relaxed);
    native_->signal_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_signal.event.store(native_->signal_event, std::memory_order_release);
    install_signal_handlers();

    // raw 模式：关行缓冲、回显与行内编辑，输入按 VT 序列逐字符到达。
    if (base::stdin_is_terminal()) {
        DWORD mode = 0;
        if (::GetConsoleMode(native_->input, &mode)) {
            native_->saved_input_mode = mode;
            native_->input_saved      = true;
            const DWORD raw =
                (mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                          ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT)) |
                ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT;
            native_->raw_saved_ = ::SetConsoleMode(native_->input, raw) != 0;
        }
    }

    // 输出开 VT 处理并固定 UTF-8 代码页；重定向时这两步都不适用。
    if (base::stdout_is_terminal()) {
        DWORD mode = 0;
        if (::GetConsoleMode(native_->output, &mode)) {
            native_->saved_output_mode = mode;
            native_->output_saved      = true;
            ::SetConsoleMode(native_->output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                                  DISABLE_NEWLINE_AUTO_RETURN);
        }
        const UINT codepage = ::GetConsoleOutputCP();
        if (codepage != 0 && ::SetConsoleOutputCP(CP_UTF8)) {
            native_->saved_output_cp = codepage;
            native_->cp_saved        = true;
        }
    }

    enter();

    g_instance.store(this, std::memory_order_release);
    std::call_once(g_atexit_once, [] { std::atexit(&terminal_atexit_restore); });
}

Terminal::~Terminal() {
    restore();
    uninstall_signal_handlers();
    g_signal.event.store(nullptr, std::memory_order_release);
    if (native_->signal_event) {
        ::CloseHandle(native_->signal_event);
        native_->signal_event = nullptr;
    }
    if (g_instance.load(std::memory_order_acquire) == this) {
        g_instance.store(nullptr, std::memory_order_release);
    }
}

// 能力固定基线；握手应答到达后由 apply_caps 修正。
void Terminal::probe_caps() noexcept {
    caps_.truecolor       = true;
    caps_.synchronized    = false;
    caps_.sgr_mouse       = true;
    caps_.bracketed_paste = true;
    caps_.kitty_keyboard  = false;
    caps_.grapheme_width  = false;
}

bool Terminal::can_query() const noexcept {
    return native_->raw_saved_ && screen_active_.load(std::memory_order_acquire);
}

Size Terminal::size() const noexcept {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (::GetConsoleScreenBufferInfo(native_->output, &info)) {
        const int cols = info.srWindow.Right - info.srWindow.Left + 1;
        const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (cols > 0 && rows > 0) {
            return {cols, rows};
        }
    }
    return {80, 24};
}

// 阻塞式写满；stdout 是文件或管道时同样按字节写。
void Terminal::write(std::string_view bytes) noexcept {
    while (!bytes.empty()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size(), std::size_t{1} << 20));
        DWORD written = 0;
        if (!::WriteFile(native_->output, bytes.data(), chunk, &written, nullptr) ||
            written == 0) {
            break;
        }
        bytes.remove_prefix(written);
    }
}

void Terminal::restore_input() noexcept {
    if (native_->input_saved) {
        ::SetConsoleMode(native_->input, native_->saved_input_mode);
        native_->input_saved = false;
        native_->raw_saved_  = false;
    }
    if (native_->output_saved) {
        ::SetConsoleMode(native_->output, native_->saved_output_mode);
        native_->output_saved = false;
    }
    if (native_->cp_saved) {
        ::SetConsoleOutputCP(native_->saved_output_cp);
        native_->cp_saved = false;
    }
}

Terminal::Signals Terminal::drain_signal() noexcept {
    Signals got;
    got.quit   = g_signal.quit_pending.exchange(false);
    got.resize = g_signal.resize_pending.exchange(false);
    if (native_->signal_event) {
        ::ResetEvent(native_->signal_event);
    }
    return got;
}

void Terminal::install_signal_handlers() noexcept {
    native_->handlers_installed_ = ::SetConsoleCtrlHandler(&terminal_console_control, TRUE) != 0;
}

void Terminal::uninstall_signal_handlers() noexcept {
    if (!native_->handlers_installed_) {
        return;
    }
    ::SetConsoleCtrlHandler(&terminal_console_control, FALSE);
    native_->handlers_installed_ = false;
}

struct EventWait::Impl {
    Terminal& terminal;
    HANDLE wake_event = nullptr;
    std::string pending;         ///< 已解码但尚未交给调用方的 UTF-8 字节
    wchar_t high_surrogate = 0;  ///< 等待低位代理的高位码元

    explicit Impl(Terminal& term) : terminal(term) {}
};

EventWait::EventWait(Terminal& terminal) : impl_(std::make_unique<Impl>(terminal)) {
    impl_->wake_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->wake_event) {
        // 唤醒事件：创建失败直接终止。
        std::terminate();
    }
}

EventWait::~EventWait() {
    if (impl_->wake_event) {
        ::CloseHandle(impl_->wake_event);
    }
}

EventWait::Ready EventWait::wait(int timeout_ms) noexcept {
    Terminal::Native& native = *impl_->terminal.native_;
    const auto ready = [](HANDLE handle) {
        return handle != nullptr && ::WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
    };

    // 解码器还有余量：立即回到调用方取走。
    if (!impl_->pending.empty()) {
        return {Ready::Status::completed, ready(native.signal_event),
                ready(impl_->wake_event), true};
    }

    // 非控制台 stdin：没有可等待的输入句柄，直接让 read_input 报告 closed。
    if (!native.raw_saved_) {
        return {Ready::Status::completed, ready(native.signal_event),
                ready(impl_->wake_event), true};
    }

    HANDLE handles[3];
    DWORD count = 0;
    const DWORD input_index = count;
    handles[count++] = native.input;
    DWORD signal_index = MAXDWORD;
    if (native.signal_event) {
        signal_index = count;
        handles[count++] = native.signal_event;
    }
    const DWORD wake_index = count;
    handles[count++] = impl_->wake_event;

    const DWORD timeout = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
    const DWORD result = ::WaitForMultipleObjects(count, handles, FALSE, timeout);
    if (result == WAIT_FAILED) {
        return {Ready::Status::failed, false, false, false};
    }
    if (result == WAIT_TIMEOUT) {
        return {Ready::Status::completed, false, false, false};
    }

    const DWORD index = result - WAIT_OBJECT_0;
    bool signal = signal_index != MAXDWORD && index == signal_index;
    bool wake   = index == wake_index;
    if (signal || wake) {
        // 手动重置事件可同时就绪，各再查一次。
        signal = ready(native.signal_event);
        wake   = ready(impl_->wake_event);
    }
    return {Ready::Status::completed, signal, wake, index == input_index};
}

EventWait::Input EventWait::read_input(std::span<char> bytes) noexcept {
    if (!impl_->pending.empty()) {
        const std::size_t count = std::min(bytes.size(), impl_->pending.size());
        if (count > 0) {
            std::memcpy(bytes.data(), impl_->pending.data(), count);
            impl_->pending.erase(0, count);
        }
        return {count, false};
    }

    Terminal::Native& native = *impl_->terminal.native_;
    if (!native.raw_saved_) {
        return {0, true};
    }

    DWORD available = 0;
    if (!::GetNumberOfConsoleInputEvents(native.input, &available)) {
        return {0, true};
    }
    if (available == 0) {
        return {0, false};
    }

    INPUT_RECORD records[128];
    DWORD read = 0;
    if (!::ReadConsoleInputW(native.input, records, 128, &read)) {
        return {0, true};
    }

    std::wstring utf16;
    // 跨记录/跨调用拼接代理对；孤立代理丢弃。
    const auto push = [&](wchar_t unit) {
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            impl_->high_surrogate = unit; // 等低位代理到达再一起输出
            return;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (impl_->high_surrogate == 0) {
                return;
            }
            utf16.push_back(impl_->high_surrogate);
            utf16.push_back(unit);
            impl_->high_surrogate = 0;
            return;
        }
        impl_->high_surrogate = 0;
        utf16.push_back(unit);
    };

    for (DWORD i = 0; i < read; ++i) {
        const INPUT_RECORD& record = records[i];
        switch (record.EventType) {
        case KEY_EVENT: {
            const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
            // UnicodeChar 为 0 的按键（修饰键等）不产生文本。
            if (!key.bKeyDown || key.uChar.UnicodeChar == 0) {
                break;
            }
            for (WORD repeat = 0; repeat < key.wRepeatCount; ++repeat) {
                push(key.uChar.UnicodeChar);
            }
            break;
        }
        case WINDOW_BUFFER_SIZE_EVENT:
            g_signal.resize_pending.store(true, std::memory_order_release);
            if (const HANDLE event = g_signal.event.load(std::memory_order_acquire)) {
                ::SetEvent(event);
            }
            break;
        case FOCUS_EVENT: {
            // 复用解码器认识的 CSI I/O 焦点事件。
            const wchar_t* seq = record.Event.FocusEvent.bSetFocus ? L"\x1b[I" : L"\x1b[O";
            for (const wchar_t* p = seq; *p != L'\0'; ++p) {
                push(*p);
            }
            break;
        }
        default:
            break; // MOUSE_EVENT / MENU_EVENT：VT 输入模式无需处理
        }
    }

    if (utf16.empty()) {
        return {0, false};
    }

    const int wide = static_cast<int>(utf16.size());
    const int size =
        ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), wide, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {0, false};
    }
    std::string utf8(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), wide, utf8.data(), size, nullptr, nullptr);

    const std::size_t count = std::min(bytes.size(), utf8.size());
    if (count > 0) {
        std::memcpy(bytes.data(), utf8.data(), count);
    }
    if (count < utf8.size()) {
        impl_->pending.append(utf8.data() + count, utf8.size() - count);
    }
    return {count, false};
}

void EventWait::wake() noexcept {
    ::SetEvent(impl_->wake_event);
}

void EventWait::drain_wake() noexcept {
    ::ResetEvent(impl_->wake_event);
}

} // namespace dagent::tui
