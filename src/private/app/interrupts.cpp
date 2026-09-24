#include "app/interrupts.hpp"

#include <csignal>
#include <thread>

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

} // namespace dagent::app
