#include "app/toolchain_detail.hpp"

#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace dagent::app::detail {
namespace fs = std::filesystem;

std::string runtime_platform() {
    utsname info{};
    if (::uname(&info) != 0) throw std::runtime_error("cannot determine runtime platform");
    return std::string(info.sysname) + "-" + info.machine;
}

struct SyncLock::Impl {
    int fd_;

    explicit Impl(const fs::path& path) {
        fs::create_directories(path.parent_path());
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd_ < 0 || ::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            if (fd_ >= 0) ::close(fd_);
            throw std::runtime_error("another runtime preparation is active");
        }
    }
    ~Impl() { ::close(fd_); }
};

SyncLock::SyncLock(const fs::path& path) : impl_(std::make_unique<Impl>(path)) {}
SyncLock::~SyncLock() = default;

} // namespace dagent::app::detail
