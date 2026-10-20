#include "app/config_detail.hpp"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>

namespace dagent::app::detail {
namespace fs = std::filesystem;

void require_private_file(const fs::path& file) {
    struct stat status {};
    if (::stat(file.c_str(), &status) != 0)
        fail(ConfigError::Kind::io, std::format("cannot stat {}: {}", file.string(), std::strerror(errno)));
    if (!S_ISREG(status.st_mode)) fail(ConfigError::Kind::io, file.string() + " is not a regular file");
    if ((status.st_mode & 0777) != 0600) {
        fail(ConfigError::Kind::invalid,
             std::format("{} must have permissions 0600 because it may contain API keys; run: chmod 600 {}",
                         file.string(), file.string()));
    }
}

} // namespace dagent::app::detail
