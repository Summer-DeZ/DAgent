#include "app/config_detail.hpp"
#include "app/home.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <system_error>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace dagent::app::detail {
namespace fs = std::filesystem;

struct ModelWriteLock::Impl {
    int fd_ = -1;
    explicit Impl(const fs::path& root);
    ~Impl();
};

ModelWriteLock::Impl::Impl(const fs::path& root) {
    const fs::path path = HomePaths(root).run / "models.lock";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) fail(ConfigError::Kind::io, "cannot create the model lock directory: " + ec.message());
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0) fail(ConfigError::Kind::io, std::format("cannot open {}: {}", path.string(), std::strerror(errno)));
    while (::flock(fd_, LOCK_EX) != 0) {
        if (errno == EINTR) continue;
        const std::string message = std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        fail(ConfigError::Kind::io, std::format("cannot lock {}: {}", path.string(), message));
    }
}

ModelWriteLock::Impl::~Impl() {
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
}

ModelWriteLock::ModelWriteLock(const fs::path& root) : impl_(std::make_unique<Impl>(root)) {}
ModelWriteLock::~ModelWriteLock() = default;

} // namespace dagent::app::detail
