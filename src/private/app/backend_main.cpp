/// @file backend_main.cpp
/// @brief dagent-backend 入口：只接受 --ipc-fd（正式前端的私有连接），不提供监听模式。
///
/// 这里是后端唯一知道具体装配的地方：把 app::assemble_backend 注入 backend::Backend。
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include "app/bootstrap.hpp"
#include "backend/backend.hpp"
#include "ipc/channel.hpp"

int main(int argc, char** argv) {
    int fd = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ipc-fd") == 0 && i + 1 < argc) {
            fd = std::atoi(argv[++i]);
        } else {
            std::cerr << "usage: dagent-backend --ipc-fd <fd>\n";
            return 2;
        }
    }
    if (fd < 0) {
        std::cerr << "usage: dagent-backend --ipc-fd <fd>\n";
        return 2;
    }

    // 前端在创建线程前屏蔽了 SIGINT/SIGTERM（sigwait 收尾）；后端恢复默认处理，
    // 独立进程组不会收到终端 SIGINT，前端宽限终止时 SIGTERM 能生效。
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    ::pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
    ::signal(SIGPIPE, SIG_IGN);

    dagent::ipc::Channel channel(fd);
    channel.set_cloexec(); // 工具/MCP 子进程不能继承私有连接
    dagent::backend::Backend backend(std::move(channel), dagent::app::assemble_backend);
    return backend.run();
}
