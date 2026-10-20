#include "storage/platform.hpp"
#include "storage/sqlite.hpp"

#include <cerrno>
#include <cstring>
#include <format>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace dagent::storage::detail {
namespace fs = std::filesystem;

struct SessionFileLock::Impl {
    int fd = -1;
};

SessionFileLock::SessionFileLock(const fs::path& path, std::string_view session_id)
    : impl_(std::make_unique<Impl>()) {
    impl_->fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (impl_->fd < 0) {
        fail(StorageError::Kind::io,
             std::format("cannot open session lock {}: {}", path.string(), std::strerror(errno)));
    }
    if (::flock(impl_->fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
        fail(StorageError::Kind::io,
             std::format("session {} is already in use by another process", session_id));
    }
}

SessionFileLock::~SessionFileLock() {
    if (impl_->fd >= 0) {
        ::flock(impl_->fd, LOCK_UN);
        ::close(impl_->fd);
    }
}

} // namespace dagent::storage::detail
