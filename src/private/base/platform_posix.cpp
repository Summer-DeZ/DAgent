#include "base/platform.hpp"
#include "base/path.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace dagent::base {

std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path(text);
}

std::string path_to_utf8(const std::filesystem::path& path) {
    return path.string();
}

std::string path_to_generic_utf8(const std::filesystem::path& path) {
    return path.generic_string();
}

std::filesystem::path executable_path() {
    return std::filesystem::read_symlink("/proc/self/exe");
}

std::filesystem::path executable_path(std::error_code& error) {
    return std::filesystem::read_symlink("/proc/self/exe", error);
}

std::optional<std::string> environment_variable(std::string_view name) {
    if (const char* value = std::getenv(std::string(name).c_str())) return std::string(value);
    return std::nullopt;
}

std::optional<std::string> home_directory() { return environment_variable("HOME"); }
std::int64_t process_id() noexcept { return ::getpid(); }
bool stdin_is_terminal() noexcept { return ::isatty(STDIN_FILENO) != 0; }
bool stdout_is_terminal() noexcept { return ::isatty(STDOUT_FILENO) != 0; }

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

std::filesystem::path default_temporary_directory() { return "/tmp"; }

void make_file_private(const std::filesystem::path& path) {
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}

void make_file_private(const std::filesystem::path& path, std::error_code& error) {
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, error);
}

std::expected<std::filesystem::path, std::string> create_unique_directory(const std::filesystem::path& pattern) {
    auto native = pattern.native();
    if (::mkdtemp(native.data()) == nullptr) return std::unexpected(std::string(std::strerror(errno)));
    return std::filesystem::path(native);
}

} // namespace dagent::base
