#include "base/platform.hpp"
#include "base/path.hpp"

#include <array>
#include <format>
#include <string_view>
#include <windows.h>
#include <bcrypt.h>

namespace dagent::base {
namespace {

std::error_code windows_error(DWORD error = ::GetLastError()) {
    return {static_cast<int>(error), std::system_category()};
}

std::wstring from_utf8(std::string_view text) {
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size == 0) throw std::system_error(windows_error());
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                              result.data(), size) == 0)
        throw std::system_error(windows_error());
    return result;
}

std::string to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0) throw std::system_error(windows_error());
    std::string result(static_cast<std::size_t>(size), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                              result.data(), size, nullptr, nullptr) == 0)
        throw std::system_error(windows_error());
    return result;
}

bool is_console(DWORD stream) noexcept {
    DWORD mode = 0;
    return ::GetConsoleMode(::GetStdHandle(stream), &mode) != 0;
}

} // namespace

std::filesystem::path path_from_utf8(std::string_view text) { return from_utf8(text); }

std::string path_to_utf8(const std::filesystem::path& path) { return to_utf8(path.native()); }

std::string path_to_generic_utf8(const std::filesystem::path& path) {
    return to_utf8(path.generic_wstring());
}

std::filesystem::path executable_path() {
    std::error_code error;
    auto result = executable_path(error);
    if (error) throw std::filesystem::filesystem_error("GetModuleFileNameW", error);
    return result;
}

std::filesystem::path executable_path(std::error_code& error) {
    std::wstring buffer(256, L'\0');
    for (;;) {
        const DWORD size = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            error = windows_error();
            return {};
        }
        if (size < buffer.size()) {
            error.clear();
            buffer.resize(size);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::optional<std::string> environment_variable(std::string_view name) {
    const auto native_name = from_utf8(name);
    std::wstring buffer(256, L'\0');
    for (;;) {
        ::SetLastError(ERROR_SUCCESS);
        const DWORD size = ::GetEnvironmentVariableW(native_name.c_str(), buffer.data(),
                                                     static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_ENVVAR_NOT_FOUND) return std::nullopt;
            if (error != ERROR_SUCCESS) throw std::system_error(windows_error(error));
            return std::string{};
        }
        if (size < buffer.size()) return to_utf8(std::wstring_view(buffer.data(), size));
        buffer.resize(size);
    }
}

std::optional<std::string> home_directory() { return environment_variable("USERPROFILE"); }
std::int64_t process_id() noexcept { return ::GetCurrentProcessId(); }
bool stdin_is_terminal() noexcept { return is_console(STD_INPUT_HANDLE); }
bool stdout_is_terminal() noexcept { return is_console(STD_OUTPUT_HANDLE); }

bool local_time(std::time_t value, std::tm& result) noexcept {
    return ::localtime_s(&result, &value) == 0;
}

bool utc_time(std::time_t value, std::tm& result) noexcept {
    return ::gmtime_s(&result, &value) == 0;
}

std::time_t utc_timestamp(std::tm& value) noexcept { return ::_mkgmtime64(&value); }

bool can_execute(const std::filesystem::path& path) noexcept {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::filesystem::path default_temporary_directory() {
    std::wstring buffer(MAX_PATH + 1, L'\0');
    for (;;) {
        const DWORD size = ::GetTempPath2W(static_cast<DWORD>(buffer.size()), buffer.data());
        if (size == 0) throw std::system_error(windows_error());
        if (size < buffer.size()) {
            buffer.resize(size);
            return std::filesystem::path(buffer);
        }
        buffer.resize(static_cast<std::size_t>(size) + 1);
    }
}

std::expected<std::filesystem::path, std::string> create_unique_directory(const std::filesystem::path& pattern) {
    auto native = pattern.native();
    if (!native.ends_with(L"XXXXXX"))
        return std::unexpected(std::make_error_code(std::errc::invalid_argument).message());
    constexpr std::wstring_view alphabet = L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::array<UCHAR, 6> random{};
    for (;;) {
        const NTSTATUS status = ::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0)
            return std::unexpected(std::format("BCryptGenRandom failed: 0x{:08x}", static_cast<ULONG>(status)));
        for (std::size_t i = 0; i < random.size(); ++i)
            native[native.size() - random.size() + i] = alphabet[random[i] % alphabet.size()];
        if (::CreateDirectoryW(native.c_str(), nullptr)) return std::filesystem::path(native);
        const DWORD error = ::GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
            return std::unexpected(windows_error(error).message());
    }
}

} // namespace dagent::base
