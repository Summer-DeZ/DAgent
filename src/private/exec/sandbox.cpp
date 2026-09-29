#include "exec/sandbox.hpp"

#include "base/log.hpp"
#include "exec/process.hpp"

#include <cerrno>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
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
    std::filesystem::path private_tmp;

    ~Impl() {
        if (ruleset_fd >= 0) ::close(ruleset_fd);
        if (!private_tmp.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(private_tmp, ec);
        }
    }
};

Prepared::Prepared() : impl_(std::make_unique<Impl>()) {}
Prepared::~Prepared() = default;
std::string_view Prepared::private_tmp() const noexcept { return impl_->private_tmp.native(); }

namespace {

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

std::uint64_t landlock_read_access() {
    return LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
}

// 普通文件只能用文件相关的位，带上目录专属位会被内核拒绝。
std::uint64_t landlock_file_access(std::uint64_t allowed) {
    std::uint64_t access = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE |
                           LANDLOCK_ACCESS_FS_WRITE_FILE;
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    access |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
    return access & allowed;
}

// 系统头文件（6.8）还没有 ABI 6 的 scoped 字段，按内核 UAPI 布局补上；
// 旧内核只要多出的字段为 0 就照常接受。
struct RulesetAttr {
    std::uint64_t handled_access_fs = 0;
    std::uint64_t handled_access_net = 0;
    std::uint64_t scoped = 0;
};
constexpr std::uint64_t landlock_scope_signal = 1ULL << 1;  ///< LANDLOCK_SCOPE_SIGNAL，ABI 6 起

int create_landlock_ruleset(std::uint64_t handled, std::uint64_t scoped) {
    RulesetAttr attr;
    attr.handled_access_fs = handled;
    attr.scoped = scoped;
    return static_cast<int>(::syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0));
}

void add_path_rule(int ruleset_fd, const std::filesystem::path& path, std::uint64_t allowed,
                   bool no_follow = false) {
    const int fd = ::open(path.c_str(), O_PATH | O_CLOEXEC | (no_follow ? O_NOFOLLOW : 0));
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
    if (S_ISLNK(st.st_mode)) {
        ::close(fd);
        return;
    }
    rule.allowed_access = S_ISDIR(st.st_mode) ? allowed : landlock_file_access(allowed);
    if (::syscall(SYS_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &rule, 0) == -1) {
        const int err = errno;
        ::close(fd);
        throw ExecError{ExecError::Kind::sandbox,
                        "landlock_add_rule " + path.string() + ": " + std::strerror(err)};
    }
    ::close(fd);
}

std::filesystem::path normalized(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, ec);
    if (ec) result = std::filesystem::absolute(path, ec).lexically_normal();
    return result;
}

bool contains(const std::filesystem::path& parent, const std::filesystem::path& child) {
    const std::filesystem::path relative = child.lexically_relative(parent);
    return !relative.empty() && (relative.begin() == relative.end() || *relative.begin() != "..");
}

bool sensitive_name(std::string_view name) {
    return name == ".env" || name.starts_with(".env.") || name.ends_with(".pem") ||
           name.ends_with(".key") || name.starts_with("id_rsa") || name.starts_with("id_ed25519");
}

void collect_sensitive(const std::filesystem::path& root,
                       std::vector<std::filesystem::path>& protected_paths) {
    const std::vector<std::filesystem::path> found = sensitive_paths(root);
    protected_paths.insert(protected_paths.end(), found.begin(), found.end());
}

void add_tree_except(int ruleset_fd, const std::filesystem::path& raw_root, std::uint64_t allowed,
                     const std::vector<std::filesystem::path>& raw_protected) {
    const std::filesystem::path root = normalized(raw_root);
    std::vector<std::filesystem::path> protected_paths;
    for (const auto& path : raw_protected) {
        const std::filesystem::path item = normalized(path);
        if (item == root) return;
        if (contains(root, item)) protected_paths.push_back(item);
    }
    if (protected_paths.empty()) {
        add_path_rule(ruleset_fd, root, allowed);
        return;
    }

    // 只授予列目录权，文件读取与所有写权限由未受保护的具体子树规则提供。
    const std::uint64_t directory_only = allowed & LANDLOCK_ACCESS_FS_READ_DIR;
    if (directory_only != 0) add_path_rule(ruleset_fd, root, directory_only);

    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::filesystem::path child = normalized(it->path());
        bool excluded = false, contains_excluded = false;
        for (const auto& item : protected_paths) {
            excluded = excluded || child == item;
            contains_excluded = contains_excluded || contains(child, item);
        }
        if (excluded) continue;
        if (contains_excluded && it->is_directory(ec))
            add_tree_except(ruleset_fd, child, allowed, protected_paths);
        else
            add_path_rule(ruleset_fd, it->path(), allowed, true);
    }
    if (ec) throw ExecError{ExecError::Kind::sandbox, "enumerate " + root.string() + ": " + ec.message()};
}

std::filesystem::path make_private_tmp() {
    std::array<char, 32> path{};
    const std::string pattern = "/tmp/dagent-command-XXXXXX";
    std::copy(pattern.begin(), pattern.end(), path.begin());
    char* created = ::mkdtemp(path.data());
    if (created == nullptr)
        throw ExecError{ExecError::Kind::sandbox, "create private temporary directory: " +
                                                     std::string(std::strerror(errno))};
    return created;
}

// 用 seccomp 拒绝 AF_INET/AF_INET6 的 socket 创建：UDP（DNS）也一起被挡住，AF_UNIX 照常。
// 只接收需要填充的几个字段，避免在非 friend 函数里提到 Prepared 的私有 Impl。
void build_filter(std::vector<sock_filter>& filter, sock_fprog& program, bool& has_filter,
                  bool allow_network, bool allow_local_sockets, bool deny_signals) {
    scmp_filter_ctx ctx = ::seccomp_init(SCMP_ACT_ALLOW);
    if (ctx == nullptr) throw ExecError{ExecError::Kind::sandbox, "seccomp_init failed"};

    const auto fail = [&](const char* step) {
        ::seccomp_release(ctx);
        throw ExecError{ExecError::Kind::sandbox, std::string(step) + ": seccomp rule error"};
    };
    if (!allow_network) {
        if (::seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(socket), 1, SCMP_A0(SCMP_CMP_EQ, AF_INET)) != 0)
            fail("deny AF_INET");
        if (::seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(socket), 1, SCMP_A0(SCMP_CMP_EQ, AF_INET6)) != 0)
            fail("deny AF_INET6");
        if (::seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(socket), 1, SCMP_A0(SCMP_CMP_EQ, AF_NETLINK)) != 0)
            fail("deny AF_NETLINK");
    }
    if (!allow_local_sockets &&
        ::seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(socket), 1,
                           SCMP_A0(SCMP_CMP_EQ, AF_UNIX)) != 0)
        fail("deny AF_UNIX");
    // io_uring 的 IORING_OP_SOCKET（内核 5.19+）不经过 socket 系统调用，会绕开上面两条规则；
    // 普通程序不依赖 io_uring，拿不到时会退回普通系统调用，所以整体禁用。
    for (const int nr : {SCMP_SYS(io_uring_setup), SCMP_SYS(io_uring_enter), SCMP_SYS(io_uring_register)}) {
        if (::seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), nr, 0) != 0) fail("deny io_uring");
    }
#ifdef __NR_socketcall
    // 32 位兼容的 socketcall 把参数放在指针里，无法按 family 过滤，直接禁止。
    if (::seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(socketcall), 0) != 0) fail("deny socketcall");
#endif

    // 不能 ptrace、读写别的进程内存或复制它的 fd。
    for (const int nr : {SCMP_SYS(ptrace), SCMP_SYS(process_vm_readv), SCMP_SYS(process_vm_writev),
                         SCMP_SYS(kcmp), SCMP_SYS(pidfd_getfd)}) {
        if (::seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), nr, 0) != 0) fail("deny process control");
    }
    // 信号边界优先交给 Landlock 的 signal scope：只能发给同一沙箱域内的进程（自己和子孙），
    // timeout、kill %1 这类管理自己子进程的用法照常工作。seccomp 分辨不出目标是不是子孙，
    // 内核没有 ABI 6 时只能整体禁止发信号。
    if (deny_signals) {
        for (const int nr : {SCMP_SYS(kill), SCMP_SYS(tkill), SCMP_SYS(tgkill), SCMP_SYS(rt_sigqueueinfo),
                             SCMP_SYS(rt_tgsigqueueinfo), SCMP_SYS(pidfd_send_signal)}) {
            if (::seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), nr, 0) != 0) fail("deny signals");
        }
    }

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

std::vector<std::filesystem::path> sensitive_paths(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return out;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (std::filesystem::is_symlink(status)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        const std::string name = it->path().filename().string();
        const bool protected_dir = std::filesystem::is_directory(status) &&
                                   (name == ".ssh" || name == ".gnupg");
        if (protected_dir || sensitive_name(name)) out.push_back(normalized(it->path()));
        if (protected_dir) it.disable_recursion_pending();
    }
    if (ec) throw ExecError{ExecError::Kind::sandbox,
                            "scan sensitive paths under " + root.string() + ": " + ec.message()};
    return out;
}

std::unique_ptr<Prepared> prepare(const Policy& policy) {
    const Support support = probe();
    auto prepared = std::make_unique<Prepared>();
    Prepared::Impl& impl = *prepared->impl_;
    const bool scope_signals = policy.mode != Mode::full_access && support.child_signals;

    if (policy.mode != Mode::full_access) {
        if (support.landlock_abi < 1)
            throw ExecError{ExecError::Kind::sandbox, "Landlock is not available on this kernel"};

        const std::uint64_t write = landlock_write_access(support.landlock_abi);
        const std::uint64_t read = policy.readable.empty() ? 0 : landlock_read_access();
        const std::uint64_t handled = write | read;
        const int ruleset_fd = create_landlock_ruleset(handled, scope_signals ? landlock_scope_signal : 0);
        if (ruleset_fd < 0)
            throw ExecError{ExecError::Kind::sandbox,
                            "landlock_create_ruleset: " + std::string(std::strerror(errno))};
        impl.ruleset_fd = ruleset_fd;

        if (read != 0) {
            static const std::filesystem::path system_read[] = {
                "/bin", "/sbin", "/usr", "/lib", "/lib64", "/etc", "/dev/null", "/dev/zero",
                "/dev/random", "/dev/urandom", "/proc/self", "/proc/thread-self", "/proc/cpuinfo",
                "/proc/meminfo", "/sys/devices/system/cpu"};
            for (const auto& path : system_read) add_path_rule(ruleset_fd, path, read);
            std::vector<std::filesystem::path> protected_read = policy.protected_read;
            if (policy.protect_sensitive_names)
                for (const auto& path : policy.readable) collect_sensitive(path, protected_read);
            for (const auto& path : policy.readable)
                add_tree_except(ruleset_fd, path, read, protected_read);
        }

        if (policy.private_tmp) {
            impl.private_tmp = make_private_tmp();
            add_path_rule(ruleset_fd, impl.private_tmp, read | write);
        }
        add_path_rule(ruleset_fd, "/dev/null", read | write);
        if (policy.mode == Mode::workspace_write) {
            const std::vector<std::filesystem::path> paths = policy.writable.empty()
                                                                  ? std::vector<std::filesystem::path>{std::filesystem::current_path()}
                                                                  : policy.writable;
            std::vector<std::filesystem::path> protected_write = policy.protected_write;
            if (policy.protect_sensitive_names)
                for (const auto& path : paths) collect_sensitive(path, protected_write);
            for (const auto& path : paths)
                add_tree_except(ruleset_fd, path, read | write, protected_write);
        }
    }

    if (!support.seccomp)
        throw ExecError{ExecError::Kind::sandbox, "seccomp is not available on this kernel"};
    build_filter(impl.filter, impl.program, impl.has_filter, policy.allow_network,
                 policy.allow_local_sockets, !scope_signals);

    impl.noop = impl.ruleset_fd < 0 && !impl.has_filter;
    return prepared;
}

Support probe() {
    Support support;
    const long abi = ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi >= 1) support.landlock_abi = static_cast<int>(abi);
    support.seccomp = ::prctl(PR_GET_SECCOMP, 0, 0, 0, 0) >= 0;
    support.backend = support.landlock_abi > 0 && support.seccomp ? "landlock-seccomp-v2" : "none";
    support.filesystem_write = support.landlock_abi >= 3;
    support.filesystem_read = support.landlock_abi >= 1;
    // Landlock 的父目录 allow 不能被子目录规则撤销；兼容路径不得声称满足嵌套保护。
    support.protected_subpaths = false;
    support.private_tmp = support.landlock_abi >= 1;
    support.network_block = support.seccomp;
    support.local_socket_block = support.seccomp;
    support.process_control_block = support.seccomp;
    support.child_signals = support.landlock_abi >= 6;
    if (!support.filesystem_write) support.missing.push_back("Landlock ABI 3 write and truncate controls");
    if (!support.filesystem_read) support.missing.push_back("Landlock read controls");
    if (!support.protected_subpaths)
        support.missing.push_back("nested protected paths inside a writable workspace");
    if (!support.seccomp) support.missing.push_back("seccomp syscall filtering");
    return support;
}

bool Support::read_only_ready() const {
    return filesystem_read && filesystem_write && private_tmp && network_block &&
           local_socket_block && process_control_block;
}

bool Support::workspace_ready() const {
    return read_only_ready() && protected_subpaths;
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
