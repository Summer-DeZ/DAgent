#include "app/interrupts.hpp"

#include <pthread.h>
#include <signal.h>

namespace dagent::app {

void initialize_backend_signals() {
    // 前端在创建线程前屏蔽了 SIGINT/SIGTERM（sigwait 收尾）；后端恢复默认处理，
    // 独立进程组不会收到终端 SIGINT，前端宽限终止时 SIGTERM 能生效。
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    ::pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
    ::signal(SIGPIPE, SIG_IGN);
}

} // namespace dagent::app
