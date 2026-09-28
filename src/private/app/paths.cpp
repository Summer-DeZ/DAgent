#include "app/paths.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace dagent::app {
namespace fs = std::filesystem;

HomePaths home_paths() {
    fs::path root;
    if (const char* value = std::getenv("DAGENT_HOME"); value != nullptr && *value != '\0') {
        std::error_code ec;
        root = fs::weakly_canonical(value, ec);
        if (ec) root = fs::absolute(value).lexically_normal();
    } else {
#ifdef DAGENT_DEV_HOME
        std::error_code ec;
        root = fs::weakly_canonical(DAGENT_DEV_HOME, ec);
        if (ec) root = fs::absolute(DAGENT_DEV_HOME).lexically_normal();
#else
        std::array<char, 4096> target{};
        const ssize_t size = ::readlink("/proc/self/exe", target.data(), target.size() - 1);
        if (size < 0)
            throw InstallError(std::format("cannot resolve /proc/self/exe: {}", std::strerror(errno)));
        root = fs::path(std::string(target.data(), static_cast<std::size_t>(size))).parent_path();
#endif
    }
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        throw InstallError("installation root does not exist or is not a directory: " + root.string());

    const fs::path probe = root / std::format(".dagent-write-{}", ::getpid());
    const int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw InstallError(std::format(
            "installation root {} is not writable (sessions and logs are stored there): {}. Set DAGENT_HOME to a writable directory",
            root.string(), std::strerror(errno)));
    }
    ::close(fd);
    ::unlink(probe.c_str());
    return HomePaths(root);
}

} // namespace dagent::app
