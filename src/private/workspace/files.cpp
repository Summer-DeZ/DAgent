#include "workspace/files.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/text.hpp"

namespace dagent::workspace {
namespace fs = std::filesystem;
namespace {

[[noreturn]] void fail(WorkspaceError::Kind kind, std::string_view op, const fs::path& p,
                       const std::string& detail = {}) {
    std::string what = std::string(op) + ": " + p.string();
    if (!detail.empty()) what += ": " + detail;
    throw WorkspaceError(kind, what);
}

std::string errno_text(int err) { return std::strerror(err); }

struct Fd {
    int value = -1;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() {
        if (value >= 0) ::close(value);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

// 前缀按路径分量逐个比较；路径都已经规范化，所以不会出现 "/work2" 被当成 "/work" 子路径。
bool is_within(const fs::path& root, const fs::path& p) {
    auto r = root.begin();
    auto q = p.begin();
    for (; r != root.end() && q != p.end(); ++r, ++q) {
        if (*r != *q) return false;
    }
    return r == root.end();
}

Stamp stamp_from_stat(const struct stat& st) {
    const auto sys = std::chrono::system_clock::time_point{std::chrono::duration_cast<
        std::chrono::system_clock::duration>(std::chrono::seconds(st.st_mtim.tv_sec) +
                                              std::chrono::nanoseconds(st.st_mtim.tv_nsec))};
    return {std::chrono::file_clock::from_sys(sys), static_cast<std::uintmax_t>(st.st_size), static_cast<std::uint64_t>(st.st_dev),
            static_cast<std::uint64_t>(st.st_ino)};
}

std::string read_all(int fd, std::size_t limit, bool& truncated, const fs::path& path) {
    std::string raw;
    raw.reserve(std::min<std::size_t>(limit, 1 << 20));
    char buf[64 << 10];
    while (raw.size() < limit) {
        const std::size_t want = std::min(sizeof(buf), limit - raw.size());
        const ssize_t n = ::read(fd, buf, want);
        if (n < 0) {
            if (errno == EINTR) continue;
            fail(WorkspaceError::Kind::io, "read_text", path, errno_text(errno));
        }
        if (n == 0) break;
        raw.append(buf, static_cast<std::size_t>(n));
    }
    if (raw.size() == limit) {
        char extra = 0;
        while (true) {
            const ssize_t n = ::read(fd, &extra, 1);
            if (n < 0 && errno == EINTR) continue;
            if (n > 0) truncated = true;
            break;
        }
    }
    return raw;
}

Eol detect_eol(std::string_view raw) {
    std::size_t crlf = 0, lf = 0, cr = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            if (i + 1 < raw.size() && raw[i + 1] == '\n') {
                ++crlf;
                ++i;
            } else {
                ++cr;
            }
        } else if (raw[i] == '\n') {
            ++lf;
        }
    }
    if (crlf + lf + cr == 0) return Eol::none;
    if (crlf > 0 && lf == 0 && cr == 0) return Eol::crlf;
    if (lf > 0 && crlf == 0 && cr == 0) return Eol::lf;
    return Eol::mixed;
}

std::string to_lf(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\r') {
            out += raw[i];
            continue;
        }
        out += '\n';
        if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
    }
    return out;
}

// 目标本身是符号链接时，落到它指向的真实文件上，而不是把链接换成普通文件。
fs::path through_symlink(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_symlink(p, ec)) return p;
    const fs::path link = fs::read_symlink(p, ec);
    if (ec) return p;
    const fs::path target = link.is_absolute() ? link : p.parent_path() / link;
    const fs::path resolved = fs::weakly_canonical(target, ec);
    return ec ? target.lexically_normal() : resolved;
}

// 新文件的权限：和 shell 重定向保持一致，按进程 umask 取 0666。umask 只能从 /proc 读，避免改全局状态。
std::uint32_t default_file_mode() {
    static const std::uint32_t mode = [] {
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line)) {
            constexpr std::string_view key = "Umask:";
            if (line.rfind(key, 0) != 0) continue;
            try {
                return 0666u & ~static_cast<std::uint32_t>(std::stoul(line.substr(key.size()), nullptr, 8));
            } catch (const std::exception&) {
                break;
            }
        }
        return 0644u;
    }();
    return mode;
}

} // namespace

Resolved resolve(const fs::path& root, std::string_view user_path) {
    std::error_code ec;
    fs::path abs_root = fs::weakly_canonical(root, ec);
    if (ec) {
        ec.clear();
        abs_root = fs::absolute(root, ec).lexically_normal();
    }
    fs::path candidate = user_path.empty() ? abs_root : fs::path(user_path);
    if (!candidate.is_absolute()) candidate = abs_root / candidate;
    fs::path resolved = fs::weakly_canonical(candidate, ec);
    if (ec) {
        ec.clear();
        resolved = fs::absolute(candidate, ec).lexically_normal();
    }
    return {resolved, is_within(abs_root, resolved)};
}

FileKind probe(const fs::path& p) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) return FileKind::missing;
        fail(WorkspaceError::Kind::io, "probe", p, errno_text(errno));
    }
    if (S_ISDIR(st.st_mode)) return FileKind::directory;

    // O_NONBLOCK 防止在 FIFO 上卡住；对普通文件没有影响。
    Fd fd(::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (fd.value < 0) fail(WorkspaceError::Kind::io, "probe", p, errno_text(errno));
    char buf[8 << 10];
    const ssize_t n = ::read(fd.value, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return FileKind::text; // FIFO 暂时没数据
        fail(WorkspaceError::Kind::io, "probe", p, errno_text(errno));
    }
    return std::memchr(buf, '\0', static_cast<std::size_t>(n)) ? FileKind::binary : FileKind::text;
}

static TextFile read_fd(int descriptor, const fs::path& p, const FileOptions& opt) {
    struct stat st{};
    if (::fstat(descriptor, &st) != 0) fail(WorkspaceError::Kind::io, "read_text", p, errno_text(errno));
    if (S_ISDIR(st.st_mode)) fail(WorkspaceError::Kind::io, "read_text", p, "is a directory");

    TextFile out;
    out.stamp = stamp_from_stat(st);
    std::string raw = read_all(descriptor, opt.max_read_bytes, out.truncated, p);
    if (raw.find('\0') != std::string::npos) fail(WorkspaceError::Kind::not_text, "read_text", p, "binary content");

    if (raw.size() >= 3 && raw[0] == '\xEF' && raw[1] == '\xBB' && raw[2] == '\xBF') {
        out.bom = true;
        raw.erase(0, 3);
    }
    out.eol = detect_eol(raw);
    std::string lf = to_lf(raw);
    if (base::is_valid_utf8(lf)) {
        out.content = std::move(lf);
    } else {
        out.content = base::to_valid_utf8(lf, &out.lossy);
    }
    return out;
}

TextFile read_text(const fs::path& p, const FileOptions& opt) {
    Fd fd(::open(p.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.value < 0) fail(WorkspaceError::Kind::io, "read_text", p, errno_text(errno));
    return read_fd(fd.value, p, opt);
}

void write_text(const fs::path& p, std::string_view content, Eol eol, bool bom,
                const std::optional<Stamp>& expect, const FileOptions& opt) {
    if (content.size() > opt.max_write_bytes) {
        fail(WorkspaceError::Kind::too_large, "write_text", p, "content exceeds max_write_bytes");
    }

    const fs::path target = through_symlink(p);
    const fs::path dir = target.parent_path().empty() ? fs::path(".") : target.parent_path();
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) fail(WorkspaceError::Kind::io, "write_text", target, "cannot create parent: " + ec.message());
    }

    std::string data;
    if (bom) data += "\xEF\xBB\xBF";
    data.reserve(data.size() + content.size() + content.size() / 16 + 16);
    if (eol == Eol::crlf) {
        for (const char c : content) {
            if (c == '\n') data += '\r';
            data += c;
        }
    } else {
        data.append(content);
    }

    struct stat cur{};
    const bool exists = ::stat(target.c_str(), &cur) == 0;
    if (!exists && errno != ENOENT && errno != ENOTDIR) {
        fail(WorkspaceError::Kind::io, "write_text", target, errno_text(errno));
    }
    if (exists && S_ISDIR(cur.st_mode)) fail(WorkspaceError::Kind::io, "write_text", target, "is a directory");
    if (expect && (!exists || !(stamp_from_stat(cur) == *expect))) {
        fail(WorkspaceError::Kind::stale, "write_text", target,
             exists ? "file changed since it was read" : "file disappeared");
    }

    // 临时文件必须和目标是同一个目录，rename 才不会跨文件系统。
    std::string temp = (dir / ("." + target.filename().string() + ".dagent-XXXXXX")).string();
    int temp_fd = ::mkstemp(temp.data());
    if (temp_fd < 0) fail(WorkspaceError::Kind::io, "write_text", target, errno_text(errno));

    struct Temp {
        std::string path;
        int fd = -1;
        bool keep = false;
        ~Temp() {
            if (fd >= 0) ::close(fd);
            if (!keep) ::unlink(path.c_str());
        }
    } tmp{temp, temp_fd};

    const mode_t mode = exists ? (cur.st_mode & 07777) : static_cast<mode_t>(default_file_mode());
    if (::fchmod(tmp.fd, mode) != 0) fail(WorkspaceError::Kind::io, "write_text", target, errno_text(errno));

    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(tmp.fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            fail(WorkspaceError::Kind::io, "write_text", target, errno_text(errno));
        }
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(tmp.fd) != 0) fail(WorkspaceError::Kind::io, "write_text", target, errno_text(errno));
    if (::close(tmp.fd) != 0) {
        tmp.fd = -1;
        fail(WorkspaceError::Kind::io, "write_text", target, errno_text(errno));
    }
    tmp.fd = -1;

    if (::rename(tmp.path.c_str(), target.c_str()) != 0) {
        fail(WorkspaceError::Kind::io, "write_text", target, errno_text(errno));
    }
    tmp.keep = true;

    Fd dfd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (dfd.value >= 0) ::fsync(dfd.value); // 目录项落盘；文件系统不支持时忽略
}

std::optional<Stamp> stamp_of(const fs::path& p) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) return std::nullopt;
        fail(WorkspaceError::Kind::io, "stat", p, errno_text(errno));
    }
    return stamp_from_stat(st);
}

namespace {
int constrained_open(int directory, const fs::path& path, int flags) {
    const open_how how{.flags = static_cast<__u64>(flags | O_CLOEXEC),
                       .mode = 0, .resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS};
    return static_cast<int>(::syscall(SYS_openat2, directory, path.c_str(), &how, sizeof(how)));
}
bool same_object(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
}

struct FileReference::Impl {
    fs::path path, anchor_path, relative;
    int anchor = -1, target = -1;
    struct stat parent_stat{}, target_stat{};
    ~Impl() { if (anchor >= 0) ::close(anchor); if (target >= 0) ::close(target); }
};

FileReference::FileReference(const fs::path& path) : impl_(std::make_unique<Impl>()) {
    auto& r = *impl_;
    r.path = path;
    r.anchor_path = path == path.root_path() ? path : path.parent_path();
    for (;;) {
        r.anchor = constrained_open(AT_FDCWD, r.anchor_path, O_PATH | O_DIRECTORY);
        if (r.anchor >= 0) break;
        if (errno != ENOENT || r.anchor_path == r.anchor_path.root_path())
            fail(WorkspaceError::Kind::stale, "prepare", path, "parent changed; prepare again");
        r.anchor_path = r.anchor_path.parent_path();
    }
    ::fstat(r.anchor, &r.parent_stat);
    r.relative = path.lexically_relative(r.anchor_path);
    r.target = constrained_open(r.anchor, r.relative, O_PATH);
    if (r.target < 0 && errno != ENOENT)
        fail(WorkspaceError::Kind::stale, "prepare", path, "target changed; prepare again");
    if (r.target >= 0) ::fstat(r.target, &r.target_stat);
}
FileReference::~FileReference() = default;
FileReference::FileReference(FileReference&&) noexcept = default;
FileReference& FileReference::operator=(FileReference&&) noexcept = default;
FileKind FileReference::kind() const {
    if (impl_->target < 0) return FileKind::missing;
    return S_ISDIR(impl_->target_stat.st_mode) ? FileKind::directory : FileKind::text;
}
std::optional<Stamp> FileReference::stamp() const {
    return impl_->target < 0 ? std::nullopt : std::optional(stamp_from_stat(impl_->target_stat));
}
void FileReference::validate() const {
    const auto& r = *impl_;
    Fd parent(constrained_open(AT_FDCWD, r.anchor_path, O_PATH | O_DIRECTORY));
    struct stat st{};
    if (parent.value < 0 || ::fstat(parent.value, &st) != 0 || !same_object(st, r.parent_stat))
        fail(WorkspaceError::Kind::stale, "access", r.path, "parent changed; prepare again");
    Fd target(constrained_open(r.anchor, r.relative, O_PATH));
    const int error = errno;
    if (r.target < 0 && target.value < 0 && error == ENOENT) return;
    if (r.target < 0 || target.value < 0 || ::fstat(target.value, &st) != 0 ||
        !same_object(st, r.target_stat))
        fail(WorkspaceError::Kind::stale, "access", r.path, "target changed; prepare again");
}
TextFile FileReference::read(const FileOptions& options) const {
    validate();
    const auto& r = *impl_;
    if (r.target < 0 || !S_ISREG(r.target_stat.st_mode))
        fail(WorkspaceError::Kind::io, "read", r.path, "not a regular file");
    // This proc reference names our already pinned inode, never a user-provided magic link.
    Fd fd(::open(("/proc/self/fd/" + std::to_string(r.target)).c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.value < 0) fail(WorkspaceError::Kind::io, "read", r.path, errno_text(errno));
    return read_fd(fd.value, r.path, options);
}
fs::path FileReference::directory_path() const {
    validate();
    if (kind() != FileKind::directory)
        fail(WorkspaceError::Kind::io, "list", impl_->path, "not a directory");
    return "/proc/self/fd/" + std::to_string(impl_->target);
}
Stamp FileReference::write(std::string_view content, Eol eol, bool bom,
                           const std::optional<Stamp>& expect, const FileOptions& options) {
    validate();
    auto& r = *impl_;
    if (content.size() > options.max_write_bytes)
        fail(WorkspaceError::Kind::too_large, "write", r.path, "content exceeds max_write_bytes");
    if (r.target >= 0 && !S_ISREG(r.target_stat.st_mode))
        fail(WorkspaceError::Kind::io, "write", r.path, "not a regular file");
    // Create missing parents relative to the approved existing ancestor, never through a link.
    int current = ::fcntl(r.anchor, F_DUPFD_CLOEXEC, 0);
    Fd parent(current);
    for (const auto& component : r.relative.parent_path()) {
        if (component == ".") continue;
        if (::mkdirat(parent.value, component.c_str(), 0777) != 0 && errno != EEXIST)
            fail(WorkspaceError::Kind::io, "mkdir", r.path, errno_text(errno));
        int next = constrained_open(parent.value, component, O_RDONLY | O_DIRECTORY);
        if (next < 0) fail(WorkspaceError::Kind::stale, "write", r.path, "parent changed; prepare again");
        ::close(parent.value);
        parent.value = next;
    }
    const std::string name = r.relative.filename().string();
    struct stat st{};
    if (expect && (r.target < 0 || ::fstat(r.target, &st) != 0 || !(stamp_from_stat(st) == *expect)))
        fail(WorkspaceError::Kind::stale, "write", r.path, "file changed since read/preview");
    std::string data = bom ? "\xEF\xBB\xBF" : "";
    for (char c : content) { if (c == '\n' && eol == Eol::crlf) data += '\r'; data += c; }
    // mkstemp through our directory FD keeps the temporary file on the approved inode.
    std::string temporary = "/proc/self/fd/" + std::to_string(parent.value) + "/.dagent-XXXXXX";
    Fd fd(::mkstemp(temporary.data()));
    if (fd.value < 0) fail(WorkspaceError::Kind::io, "write", r.path, errno_text(errno));
    const std::string temp_name = fs::path(temporary).filename();
    struct Unlink { int fd; std::string name; ~Unlink() { ::unlinkat(fd, name.c_str(), 0); } } cleanup{parent.value, temp_name};
    if (::fchmod(fd.value, r.target >= 0 ? (r.target_stat.st_mode & 0777) : default_file_mode()) != 0)
        fail(WorkspaceError::Kind::io, "write", r.path, errno_text(errno));
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd.value, data.data() + written, data.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail(WorkspaceError::Kind::io, "write", r.path, errno_text(errno));
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd.value) != 0) fail(WorkspaceError::Kind::io, "write", r.path, errno_text(errno));
    validate();
    if (expect && (::fstat(r.target, &st) != 0 || !(stamp_from_stat(st) == *expect)))
        fail(WorkspaceError::Kind::stale, "write", r.path, "file changed since preview");
    const unsigned flags = r.target < 0 ? RENAME_NOREPLACE : 0;
    if (::renameat2(parent.value, temp_name.c_str(), parent.value, name.c_str(), flags) != 0)
        fail(WorkspaceError::Kind::stale, "write", r.path, "target changed or cannot be replaced");
    Fd directory(::openat(parent.value, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.value >= 0) ::fsync(directory.value);
    ::fstat(fd.value, &st);
    return stamp_from_stat(st);
}

} // namespace dagent::workspace
