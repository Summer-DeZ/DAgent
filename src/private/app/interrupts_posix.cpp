#include "app/interrupts.hpp"

#include <csignal>
#include <thread>

#include <pthread.h>
#include <signal.h>

namespace dagent::app {

Interrupts& install_interrupts() {
    static Interrupts* const state = new Interrupts; // 永不释放：detach 的 sigwait 线程在进程退出前一直引用它
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    ::pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::thread([set] {
        int sig = 0;
        if (::sigwait(&set, &sig) != 0) return;
        if (!state->graceful.load()) std::_Exit(130); // 轮外没有需要收尾的东西
        state->stop.request_stop();
        if (::sigwait(&set, &sig) == 0) std::_Exit(130);
    }).detach();
    return *state;
}

// docs/design/app.md §6：整个进程忽略 SIGPIPE，写关闭的管道得到 EPIPE 而不是被信号杀死。
// exec 层只会在 SIGPIPE 仍是默认处理时设置它，两者不冲突。
void ignore_broken_pipe() { ::signal(SIGPIPE, SIG_IGN); }

} // namespace dagent::app
