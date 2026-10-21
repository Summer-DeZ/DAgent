#include "ui/browser.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>

extern char** environ;

namespace dagent::ui {
bool open_browser(const std::string& url) {
#ifdef __APPLE__
    const char* program = "open";
#else
    const char* program = "xdg-open";
#endif
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd = 0; fd < 3; ++fd)
        posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY, 0);
    posix_spawn_file_actions_addclosefrom_np(&actions, 3); // 日志文件等不进入浏览器进程
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t mask;
    sigemptyset(&mask);
    posix_spawnattr_setsigmask(&attributes, &mask);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSID);
#else
    posix_spawnattr_setpgroup(&attributes, 0);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP);
#endif
    char* argv[] = {const_cast<char*>(program), const_cast<char*>(url.c_str()), nullptr};
    pid_t pid = 0;
    const int result = posix_spawnp(&pid, program, &actions, &attributes, argv, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) return false;
    std::thread([pid] { while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {} }).detach();
    return true;
}
}
