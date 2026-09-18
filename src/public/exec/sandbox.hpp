/// @file sandbox.hpp
/// @brief OS 隔离：Landlock 限制文件系统写入，seccomp 禁止网络（AF_INET/AF_INET6 socket 与 io_uring）。
///
/// 需要内存分配的工作（打开路径、创建规则集、生成 BPF）全部在父进程的 prepare 里完成；
/// apply_in_child 只调用 prctl/syscall，可以在 fork 之后、exec 之前安全执行。
/// 规则只对 exec 出来的程序生效并继承给它的所有子孙，绝不能在本进程里调用。
#pragma once

#include <filesystem>
#include <memory>
#include <vector>

namespace dagent::exec {

/// 三档权限，和 codex 对应。
enum class Mode { read_only, workspace_write, full_access };

struct Policy {
    Mode mode = Mode::workspace_write;
    /// 可写路径；workspace_write 模式为空时用当前目录 + /tmp + /dev/null。
    /// /dev/null 始终可写，否则大量程序会莫名失败。
    std::vector<std::filesystem::path> writable;
    bool allow_network = false;
};

struct Prepared;

namespace detail {

/// @internal 只允许在子进程的 on_exec_setup 钩子里调用；返回 0 或 errno，不做任何内存分配。
int apply_in_child(const Prepared& prepared) noexcept;

} // namespace detail

/// 不透明句柄：父进程准备好的路径 fd、Landlock 规则集和 seccomp BPF 程序。
struct Prepared {
    Prepared();
    ~Prepared();
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;

private:
    friend int detail::apply_in_child(const Prepared&) noexcept;
    friend std::unique_ptr<Prepared> prepare(const Policy&);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief 在父进程里准备好所有内核对象；失败抛 ExecError{Kind::sandbox}。返回值在 run() 期间必须存活。
std::unique_ptr<Prepared> prepare(const Policy& policy);

/// @brief 启动时探测一次：不支持的机器上核心要降级为「必须询问用户」。
struct Support {
    int landlock_abi = 0;  ///< 0 表示内核不支持 Landlock
    bool seccomp = false;
};
Support probe();

} // namespace dagent::exec
