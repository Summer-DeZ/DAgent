#include "llm/auth.hpp"
#include "llm/auth_detail.hpp"
#include "base/platform.hpp"

#include <cerrno>
#include <fcntl.h>
#include <format>
#include <random>
#include <utility>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dagent::llm {
namespace {
using json = nlohmann::json;
using namespace std::chrono_literals;
struct Fd {
    int value = -1;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
[[noreturn]] void io_error() { throw std::runtime_error("credential storage is unavailable or unsafe"); }
void check_file(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0777) != 0600)
        io_error();
}
int directory(const std::filesystem::path& path, bool create) {
    Fd current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (current.value < 0) io_error();
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    for (const auto& part : absolute.relative_path()) {
        int next = ::openat(current.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT) {
            if (!create) return -1;
            if (::mkdirat(current.value, part.c_str(), 0700) && errno != EEXIST) io_error();
            next = ::openat(current.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) io_error();
        ::close(current.value);
        current.value = next;
    }
    struct stat st{};
    if (::fstat(current.value, &st) || st.st_uid != ::geteuid() || (st.st_mode & 0777) != 0700) io_error();
    return std::exchange(current.value, -1);
}
} // namespace

namespace detail {
struct CredentialLock::Impl {
    Fd dir, file;
    Impl(const CredentialPaths& paths, std::stop_token stop)
        : dir(directory(paths.lock.parent_path(), true)),
          file(::openat(dir.value, paths.lock.filename().c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600)) {
        if (file.value < 0) io_error();
        check_file(file.value);
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        for (;;) {
            check_stop(stop);
            if (::flock(file.value, LOCK_EX | LOCK_NB) == 0) break;
            if (errno != EWOULDBLOCK && errno != EINTR) io_error();
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("credential lock wait timed out");
            pause(50ms, stop);
        }
    }
};

CredentialLock::CredentialLock(const CredentialPaths& paths, std::stop_token stop)
    : impl_(std::make_unique<Impl>(paths, stop)) {}
CredentialLock::~CredentialLock() = default;

void write_credentials(const CredentialPaths& paths, const Credentials& value) {
    Fd dir(directory(paths.file.parent_path(), true));
    const auto name = paths.file.filename().string();
    Fd existing(::openat(dir.value, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (existing.value >= 0) check_file(existing.value);
    else if (errno != ENOENT) io_error();
    const auto temp = std::format(".{}-{}-{}", name, base::process_id(), std::random_device{}());
    Fd fd(::openat(dir.value, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (fd.value < 0) io_error();
    try {
        const auto data = credential_json(value).dump() + "\n";
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto n = ::write(fd.value, data.data() + offset, data.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) io_error();
            offset += static_cast<std::size_t>(n);
        }
        if (::fsync(fd.value) || ::renameat(dir.value, temp.c_str(), dir.value, name.c_str()) || ::fsync(dir.value)) io_error();
    } catch (...) { ::unlinkat(dir.value, temp.c_str(), 0); throw; }
}
void remove_credentials(const CredentialPaths& paths) {
    Fd dir(directory(paths.file.parent_path(), false));
    if (::unlinkat(dir.value, paths.file.filename().c_str(), 0) || ::fsync(dir.value)) io_error();
}

} // namespace detail

std::optional<Credentials> read_credentials(const CredentialPaths& paths) {
    Fd dir(directory(paths.file.parent_path(), false));
    if (dir.value < 0) return std::nullopt;
    Fd fd(::openat(dir.value, paths.file.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (fd.value < 0) { if (errno == ENOENT) return std::nullopt; io_error(); }
    check_file(fd.value);
    std::string data;
    char buffer[4096];
    for (;;) {
        const auto n = ::read(fd.value, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || data.size() + static_cast<std::size_t>(n) > 64 * 1024) io_error();
        if (n == 0) break;
        data.append(buffer, static_cast<std::size_t>(n));
    }
    try {
        return detail::decode_credentials(data);
    } catch (const json::exception&) {
        Credentials invalid;
        invalid.state = "invalid";
        return invalid;
    }
}
} // namespace dagent::llm
