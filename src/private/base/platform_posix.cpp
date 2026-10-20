#include "base/platform.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace dagent::base {

std::filesystem::path executable_path() {
    return std::filesystem::read_symlink("/proc/self/exe");
}

std::filesystem::path executable_path(std::error_code& error) {
    return std::filesystem::read_symlink("/proc/self/exe", error);
}

const char* home_directory() noexcept { return std::getenv("HOME"); }
std::int64_t process_id() noexcept { return ::getpid(); }
bool stdin_is_terminal() noexcept { return ::isatty(STDIN_FILENO) != 0; }

bool local_time(std::time_t value, std::tm& result) noexcept {
    return ::localtime_r(&value, &result) != nullptr;
}

bool utc_time(std::time_t value, std::tm& result) noexcept {
    return ::gmtime_r(&value, &result) != nullptr;
}

std::time_t utc_timestamp(std::tm& value) noexcept { return ::timegm(&value); }

bool can_execute(const std::filesystem::path& path) noexcept {
    return ::access(path.c_str(), X_OK) == 0;
}

std::expected<std::filesystem::path, std::string> create_unique_directory(std::string pattern) {
    if (::mkdtemp(pattern.data()) == nullptr) return std::unexpected(std::string(std::strerror(errno)));
    return std::filesystem::path(pattern);
}

} // namespace dagent::base
