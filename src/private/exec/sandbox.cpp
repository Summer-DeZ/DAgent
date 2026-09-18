#include "exec/sandbox.hpp"

#include "base/log.hpp"
#include "exec/process.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <fcntl.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>

namespace dagent::exec {

// 父进程准备好的全部内核对象。子进程只用其中的 fd 和 BPF 数组。
struct Prepared::Impl {
    bool noop = false;
    int ruleset_fd = -1;              ///< Landlock 规则集，子进程 restrict 后自己关闭
    std::vector<sock_filter> filter;  ///< seccomp BPF 程序，随 Prepared 存活到 fork 之后
    sock_fprog program {};            ///< 指向 filter 的程序头，避免在子进程里组装
    bool has_filter = false;

    ~Impl() {
        if (ruleset_fd >= 0) ::close(ruleset_fd);
    }
};

Prepared::Prepared() : impl_(std::make_unique<Impl>()) {}
Prepared::~Prepared() = default;

namespace {

// 只处理「写」类权限：读和执行的访问规则不进入 handled 集合，因此全文件系统可读可执行。
std::uint64_t landlock_write_access(int abi) {
    std::uint64_t access = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                           LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_MAKE_CHAR |
                           LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                           LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
                           LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2) access |= LANDLOCK_ACCESS_FS_REFER;  // ABI 2 才有
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) access |= LANDLOCK_ACCESS_FS_TRUNCATE;  // ABI 3 才有
#endif
    return access;
}

// 普通文件只能用文件相关的位，带上目录专属位会被内核拒绝。
std::uint64_t landlock_file_access(std::uint64_t handled) {
    std::uint64_t access = LANDLOCK_ACCESS_FS_WRITE_FILE;
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    access |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
    return access & handled;
}

int create_landlock_ruleset(std::uint64_t handled) {
    landlock_ruleset_attr attr {};
    attr.handled_access_fs = handled;
    return static_cast<int>(::syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0));
}

void add_path_rule(int ruleset_fd, const std::filesystem::path& path, std::uint64_t handled) {
    const int fd = ::open(path.c_str(), O_PATH | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            base::logger("exec")->warn("sandbox path does not exist, skipped: {}", path.string());
            return;
        }
        throw ExecError{ExecError::Kind::sandbox, "open " + path.string() + ": " + std::strerror(errno)};
    }

    struct stat st {};
    if (::fstat(fd, &st) == -1) {
        const int err = errno;
        ::close(fd);
        throw ExecError{ExecError::Kind::sandbox, "stat " + path.string() + ": " + std::strerror(err)};
    }

    landlock_path_beneath_attr rule {};
    rule.parent_fd = fd;
    rule.allowed_access = S_ISDIR(st.st_mode) ? handled : landlock_file_access(handled);
    if (::syscall(SYS_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &rule, 0) == -1) {
        const int err = errno;
        ::close(fd);
        throw ExecError{ExecError::Kind::sandbox,
                        "landlock_add_rule " + path.string() + ": " + std::strerror(err)};
    }
    ::close(fd);
}

// 用 seccomp 拒绝 AF_INET/AF_INET6 的 socket 创建：UDP（DNS）也一起被挡住，AF_UNIX 照常。
// 只接收需要填充的几个字段，避免在非 friend 函数里提到 Prepared 的私有 Impl。
void build_network_filter(std::vector<sock_filter>& filter, sock_fprog& program, bool& has_filter) {
    scmp_filter_ctx ctx = ::seccomp_init(SCMP_ACT_ALLOW);
    if (ctx == nullptr) throw ExecError{ExecError::Kind::sandbox, "seccomp_init failed"};

    const auto fail = [&](const char* step) {
        ::seccomp_release(ctx);
        throw ExecError{ExecError::Kind::sandbox, std::string(step) + ": seccomp rule error"};
    };
    if (::seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(socket), 1, SCMP_A0(SCMP_CMP_EQ, AF_INET)) != 0)
        fail("deny AF_INET");
    if (::seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(socket), 1, SCMP_A0(SCMP_CMP_EQ, AF_INET6)) != 0)
        fail("deny AF_INET6");
    // io_uring 的 IORING_OP_SOCKET（内核 5.19+）不经过 socket 系统调用，会绕开上面两条规则；
    // 普通程序不依赖 io_uring，拿不到时会退回普通系统调用，所以整体禁用。
    for (const int nr : {SCMP_SYS(io_uring_setup), SCMP_SYS(io_uring_enter), SCMP_SYS(io_uring_register)}) {
        if (::seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), nr, 0) != 0) fail("deny io_uring");
    }
#ifdef __NR_socketcall
    // 32 位兼容的 socketcall 把参数放在指针里，无法按 family 过滤，直接禁止。
    if (::seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(socketcall), 0) != 0) fail("deny socketcall");
#endif

    // 2.5.5 没有 seccomp_export_bpf_mem，先导出到 memfd 再读回成 sock_filter 数组；
    // 子进程里只需 prctl 加载，不再调用会分配内存的 libseccomp。
    const int memfd = ::memfd_create("dagent-seccomp", MFD_CLOEXEC);
    if (memfd < 0) fail("memfd_create");
    if (::seccomp_export_bpf(ctx, memfd) != 0) {
        const int err = errno;
        ::close(memfd);
        ::seccomp_release(ctx);
        throw ExecError{ExecError::Kind::sandbox, "seccomp_export_bpf: " + std::string(std::strerror(err))};
    }

    const off_t size = ::lseek(memfd, 0, SEEK_END);
    if (size <= 0 || size % static_cast<off_t>(sizeof(sock_filter)) != 0) {
        ::close(memfd);
        ::seccomp_release(ctx);
        throw ExecError{ExecError::Kind::sandbox, "seccomp BPF program has unexpected size"};
    }
    filter.resize(static_cast<std::size_t>(size) / sizeof(sock_filter));
    if (::pread(memfd, filter.data(), static_cast<std::size_t>(size), 0) != size) {
        const int err = errno;
        ::close(memfd);
        ::seccomp_release(ctx);
        throw ExecError{ExecError::Kind::sandbox, "read seccomp BPF: " + std::string(std::strerror(err))};
    }
    ::close(memfd);
    ::seccomp_release(ctx);
    program.len = static_cast<unsigned short>(filter.size());
    program.filter = filter.data();
    has_filter = true;
}

} // namespace

std::unique_ptr<Prepared> prepare(const Policy& policy) {
    const Support support = probe();
    auto prepared = std::make_unique<Prepared>();
    Prepared::Impl& impl = *prepared->impl_;

    if (policy.mode != Mode::full_access) {
        if (support.landlock_abi < 1)
            throw ExecError{ExecError::Kind::sandbox, "Landlock is not available on this kernel"};

        const std::uint64_t handled = landlock_write_access(support.landlock_abi);
        const int ruleset_fd = create_landlock_ruleset(handled);
        if (ruleset_fd < 0)
            throw ExecError{ExecError::Kind::sandbox,
                            "landlock_create_ruleset: " + std::string(std::strerror(errno))};
        impl.ruleset_fd = ruleset_fd;

        std::vector<std::filesystem::path> paths;
        if (policy.mode == Mode::read_only) {
            // 只读模式也要放行 /dev/null 和 /proc/self，否则很多程序会莫名失败。
            paths = {"/dev/null", "/proc/self"};
        } else if (policy.writable.empty()) {
            paths = {std::filesystem::current_path(), "/tmp", "/dev/null"};
        } else {
            paths = policy.writable;
            paths.push_back("/dev/null");
        }
        for (const auto& path : paths) add_path_rule(ruleset_fd, path, handled);
    }

    if (!policy.allow_network) {
        if (!support.seccomp)
            throw ExecError{ExecError::Kind::sandbox, "seccomp is not available on this kernel"};
        build_network_filter(impl.filter, impl.program, impl.has_filter);
    }

    impl.noop = impl.ruleset_fd < 0 && !impl.has_filter;
    return prepared;
}

Support probe() {
    Support support;
    const long abi = ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi >= 1) support.landlock_abi = static_cast<int>(abi);
    support.seccomp = ::prctl(PR_GET_SECCOMP, 0, 0, 0, 0) >= 0;
    return support;
}

namespace detail {

int apply_in_child(const Prepared& prepared) noexcept {
    const Prepared::Impl& impl = *prepared.impl_;
    if (impl.noop) return 0;

    // fork 之后只能做无分配的调用：prctl / syscall / close。
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1) return errno;

    if (impl.ruleset_fd >= 0) {
        if (::syscall(SYS_landlock_restrict_self, impl.ruleset_fd, 0) == -1) return errno;
        ::close(impl.ruleset_fd);
    }
    if (impl.has_filter) {
        if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &impl.program) == -1) return errno;
    }
    return 0;
}

} // namespace detail

} // namespace dagent::exec
