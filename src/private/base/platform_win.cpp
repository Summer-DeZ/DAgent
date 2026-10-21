#include "base/platform.hpp"
#include "base/path.hpp"
#include "base/security_detail.hpp"

#include <array>
#include <format>
#include <string_view>
#include <memory>
#include <vector>
#include <windows.h>
#include <bcrypt.h>
#include <aclapi.h>

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

namespace dagent::base {
namespace {

struct CloseHandleDeleter { void operator()(void* handle) const { ::CloseHandle(handle); } };
struct LocalFreeDeleter { void operator()(void* memory) const { ::LocalFree(memory); } };

void security_check(BOOL success) {
    if (!success) throw std::system_error(windows_error());
}

struct Principals {
    std::vector<std::byte> token_user;
    std::array<BYTE, SECURITY_MAX_SID_SIZE> system_sid{}, administrators_sid{};
    PSID user() const { return reinterpret_cast<const TOKEN_USER*>(token_user.data())->User.Sid; }
    Principals() {
        HANDLE raw = nullptr;
        security_check(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw));
        std::unique_ptr<void, CloseHandleDeleter> token(raw);
        DWORD size = 0;
        ::GetTokenInformation(raw, TokenUser, nullptr, 0, &size);
        token_user.resize(size);
        security_check(::GetTokenInformation(raw, TokenUser, token_user.data(), size, &size));
        size = static_cast<DWORD>(system_sid.size());
        security_check(::CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid.data(), &size));
        size = static_cast<DWORD>(administrators_sid.size());
        security_check(::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators_sid.data(), &size));
    }
    bool trusted(PSID sid) {
        return ::EqualSid(sid, user()) || ::EqualSid(sid, system_sid.data()) || ::EqualSid(sid, administrators_sid.data());
    }
};

} // namespace

std::vector<std::byte> detail::private_security_descriptor() {
    Principals principals;
    PSID sids[] = {principals.user(), principals.system_sid.data(), principals.administrators_sid.data()};
    EXPLICIT_ACCESS_W entries[3]{};
    for (std::size_t i = 0; i < 3; ++i) {
        entries[i].grfAccessPermissions = FILE_ALL_ACCESS;
        entries[i].grfAccessMode = SET_ACCESS;
        entries[i].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entries[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[i].Trustee.ptstrName = static_cast<LPWSTR>(sids[i]);
    }
    PACL raw_acl = nullptr;
    const DWORD error = ::SetEntriesInAclW(3, entries, nullptr, &raw_acl);
    if (error != ERROR_SUCCESS) throw std::system_error(windows_error(error));
    std::unique_ptr<void, LocalFreeDeleter> acl(raw_acl);
    SECURITY_DESCRIPTOR descriptor{};
    security_check(::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION));
    security_check(::SetSecurityDescriptorOwner(&descriptor, principals.user(), FALSE));
    security_check(::SetSecurityDescriptorDacl(&descriptor, TRUE, raw_acl, FALSE));
    security_check(::SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED));
    DWORD size = 0;
    ::MakeSelfRelativeSD(&descriptor, nullptr, &size);
    std::vector<std::byte> result(size);
    security_check(::MakeSelfRelativeSD(&descriptor, result.data(), &size));
    return result;
}

bool detail::is_private_security_descriptor(std::span<const std::byte> bytes, bool directory, bool require_current_owner) {
    if (bytes.empty()) return false;
    auto descriptor = const_cast<std::byte*>(bytes.data());
    if (!::IsValidSecurityDescriptor(descriptor)) return false;
    Principals principals;
    PSID owner = nullptr;
    BOOL defaulted = FALSE, present = FALSE;
    PACL acl = nullptr;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    security_check(::GetSecurityDescriptorOwner(descriptor, &owner, &defaulted));
    security_check(::GetSecurityDescriptorControl(descriptor, &control, &revision));
    security_check(::GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted));
    if (!owner || !(require_current_owner ? ::EqualSid(owner, principals.user()) : principals.trusted(owner)) ||
        !(control & SE_DACL_PROTECTED) || !present || !acl) return false;
    DWORD granted = 0;
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void* raw = nullptr;
        security_check(::GetAce(acl, i, &raw));
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        // 条件 ACE、对象 ACE 和 deny 组合不作猜测；私有模板只生成普通 allow ACE。
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        PSID sid = const_cast<DWORD*>(&ace->SidStart);
        if (!principals.trusted(sid)) return false;
        if (::EqualSid(sid, principals.user()) && !(header->AceFlags & INHERIT_ONLY_ACE)) granted |= ace->Mask;
    }
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    ::MapGenericMask(&granted, &mapping);
    const DWORD required = directory ? FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_TRAVERSE
                                     : FILE_READ_DATA | FILE_WRITE_DATA;
    return (granted & required) == required;
}

void make_file_private(const std::filesystem::path& path, std::error_code& error) {
    try {
        auto descriptor = detail::private_security_descriptor();
        BOOL present = FALSE, defaulted = FALSE;
        PACL acl = nullptr;
        security_check(::GetSecurityDescriptorDacl(descriptor.data(), &present, &acl, &defaulted));
        HANDLE raw = ::CreateFileW(path.c_str(), WRITE_DAC | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (raw == INVALID_HANDLE_VALUE) { error = windows_error(); return; }
        std::unique_ptr<void, CloseHandleDeleter> handle(raw);
        FILE_ATTRIBUTE_TAG_INFO info{};
        security_check(::GetFileInformationByHandleEx(raw, FileAttributeTagInfo, &info, sizeof(info)));
        if (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { error = windows_error(ERROR_CANT_ACCESS_FILE); return; }
        const DWORD code = ::SetSecurityInfo(raw, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
        if (code != ERROR_SUCCESS) error = windows_error(code);
        else error.clear();
    } catch (const std::system_error& exception) { error = exception.code(); }
}

void make_file_private(const std::filesystem::path& path) {
    std::error_code error;
    make_file_private(path, error);
    if (error) throw std::filesystem::filesystem_error("cannot set private file permissions", path, error);
}

} // namespace dagent::base
