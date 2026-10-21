#include "app/interrupts.hpp"

#include <system_error>
#include <thread>
#include <windows.h>

namespace dagent::app {
namespace {

struct ConsoleInterrupts {
    Interrupts state;
    HANDLE event = nullptr;
    volatile LONG count = 0;
};

// 与 POSIX 的 detached sigwait 线程相同，状态及事件持续到进程退出。
ConsoleInterrupts* console = nullptr;

BOOL WINAPI handle_control(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    const LONG count = ::InterlockedIncrement(&console->count);
    if (!console->state.graceful.load() || count > 1) {
        // 不进入 CRT/DLL 收尾，也不等待第一回调或 request_stop 中的用户回调返回。
        ::TerminateProcess(::GetCurrentProcess(), 130);
        return TRUE;
    }
    ::SetEvent(console->event);
    return TRUE;
}

} // namespace

Interrupts& install_interrupts() {
    static ConsoleInterrupts* const state = new ConsoleInterrupts;
    state->event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state->event)
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "CreateEventW interrupts");
    console = state;
    if (!::SetConsoleCtrlHandler(handle_control, TRUE))
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "SetConsoleCtrlHandler");
    std::thread([control = state] {
        if (::WaitForSingleObject(control->event, INFINITE) == WAIT_OBJECT_0) control->state.stop.request_stop();
    }).detach();
    return state->state;
}

void ignore_broken_pipe() {}

} // namespace dagent::app
