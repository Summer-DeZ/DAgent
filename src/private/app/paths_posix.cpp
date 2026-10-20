#include "app/paths.hpp"
#include "base/platform.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <fcntl.h>
#include <unistd.h>

namespace dagent::app::detail {
namespace fs = std::filesystem;

fs::path executable_home_directory() {
    std::array<char, 4096> target{};
    const ssize_t size = ::readlink("/proc/self/exe", target.data(), target.size() - 1);
    if (size < 0)
        throw InstallError(std::format("cannot resolve /proc/self/exe: {}", std::strerror(errno)));
    return fs::path(std::string(target.data(), static_cast<std::size_t>(size))).parent_path();
}

void probe_home_writable(const fs::path& root) {
    const fs::path probe = root / std::format(".dagent-write-{}", base::process_id());
    const int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw InstallError(std::format(
            "installation root {} is not writable (sessions and logs are stored there): {}. Set DAGENT_HOME to a writable directory",
            root.string(), std::strerror(errno)));
    }
    ::close(fd);
    ::unlink(probe.c_str());
}

} // namespace dagent::app::detail
