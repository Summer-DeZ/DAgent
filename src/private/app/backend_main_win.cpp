#include "app/interrupts.hpp"

#include <system_error>
#include <windows.h>

namespace dagent::app {
namespace {

BOOL WINAPI ignore_frontend_control(DWORD event) {
    return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
}

} // namespace

void initialize_backend_signals() {
    // 控制台取消由前端经 IPC 传入；后端退出及子孙进程清理由连接和生命周期 Job 决定。
    if (!::SetConsoleCtrlHandler(ignore_frontend_control, TRUE))
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "SetConsoleCtrlHandler backend");
}

} // namespace dagent::app
