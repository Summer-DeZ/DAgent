#include "llm/auth.hpp"
#include "llm/auth_detail.hpp"
#include "base/security_detail.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>
#include <windows.h>
#include <winternl.h>
#include <aclapi.h>
#include <bcrypt.h>

namespace dagent::llm {
namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value);
            value = std::exchange(other.value, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};
struct LocalFreeDeleter { void operator()(void* memory) const { ::LocalFree(memory); } };
[[noreturn]] void io_error() { throw std::runtime_error("credential storage is unavailable or unsafe"); }

constexpr DWORD metadata_access = FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
constexpr DWORD file_sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

void check_kind(HANDLE handle, bool directory) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (::GetFileType(handle) != FILE_TYPE_DISK ||
        !::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) ||
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        static_cast<bool>(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory) io_error();
}

void check_private(HANDLE handle, bool directory) {
    check_kind(handle, directory);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD code = ::GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                          nullptr, nullptr, nullptr, nullptr, &descriptor);
    if (code != ERROR_SUCCESS) io_error();
    std::unique_ptr<void, LocalFreeDeleter> owner(descriptor);
    if (!base::detail::is_private_security_descriptor(
            {static_cast<const std::byte*>(descriptor), ::GetSecurityDescriptorLength(descriptor)}, directory, true)) io_error();
}

// WDK NtCreateFile: OPEN=1、CREATE=2、OPEN_IF=3；同步 I/O=0x20、OPEN_REPARSE_POINT=0x200000。
// https://learn.microsoft.com/windows/win32/api/winternl/nf-winternl-ntcreatefile
Handle open_relative(HANDLE parent, const fs::path& component, DWORD access, bool directory,
                     ULONG disposition, PSECURITY_DESCRIPTOR security, DWORD& error, DWORD share = file_sharing) {
    const auto& name = component.native();
    if (name.empty() || name == L".." || name.find_first_of(L"\\/:") != std::wstring::npos ||
        name.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t)) io_error();
    UNICODE_STRING text{};
    text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    text.MaximumLength = text.Length;
    text.Buffer = const_cast<wchar_t*>(name.data());
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &text;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    attributes.SecurityDescriptor = security;
    IO_STATUS_BLOCK status_block{};
    HANDLE raw = INVALID_HANDLE_VALUE;
    const NTSTATUS status = ::NtCreateFile(&raw, access, &attributes, &status_block, nullptr,
        directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
        directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : share, disposition,
        0x00000020 | 0x00200000 | (directory ? 0x00000001 : 0x00000040), nullptr, 0);
    if (status < 0) { error = ::RtlNtStatusToDosError(status); return Handle{}; }
    Handle result(raw);
    check_kind(raw, directory);
    error = ERROR_SUCCESS;
    return result;
}

Handle directory(const fs::path& path, bool create) {
    auto descriptor = create ? base::detail::private_security_descriptor() : std::vector<std::byte>{};
    const auto absolute = fs::absolute(path).lexically_normal();
    Handle current(::CreateFileW(absolute.root_path().c_str(), metadata_access, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!current) io_error();
    check_kind(current.value, true);
    for (const auto& part : absolute.relative_path()) {
        if (part == L"." || part.empty()) continue;
        DWORD error = ERROR_SUCCESS;
        Handle next = open_relative(current.value, part, metadata_access, true, create ? 3 : 1,
                                     create ? descriptor.data() : nullptr, error);
        if (!next) {
            if (!create && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) return Handle{};
            io_error();
        }
        current = std::move(next);
    }
    check_private(current.value, true);
    return current;
}

std::wstring temporary_name(const fs::path& file) {
    std::array<UCHAR, 16> random{};
    if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) io_error();
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring name = L"." + file.filename().native() + L"-" + std::to_wstring(::GetCurrentProcessId()) + L"-";
    for (UCHAR byte : random) { name += hex[byte >> 4]; name += hex[byte & 15]; }
    return name;
}

void rename_file(HANDLE file, HANDLE parent, const fs::path& name) {
    const auto& text = name.native();
    const auto bytes = text.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + bytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    // FileRenameInfoEx 的 REPLACE_IF_EXISTS | POSIX_SEMANTICS；保留已有检查句柄。
    rename->Flags = 0x00000001 | 0x00000002;
    rename->RootDirectory = parent;
    rename->FileNameLength = static_cast<DWORD>(bytes);
    std::memcpy(rename->FileName, text.data(), bytes);
    if (!::SetFileInformationByHandle(file, FileRenameInfoEx, rename, static_cast<DWORD>(buffer.size()))) io_error();
}

struct Temporary {
    Handle file;
    bool published = false;
    ~Temporary() {
        if (file && !published) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            ::SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
        }
    }
};

} // namespace

namespace detail {
struct CredentialLock::Impl {
    Handle dir, file;
    Impl(const CredentialPaths& paths, std::stop_token stop) : dir(directory(paths.lock.parent_path(), true)) {
        auto descriptor = base::detail::private_security_descriptor();
        DWORD error = ERROR_SUCCESS;
        file = open_relative(dir.value, paths.lock.filename(), GENERIC_READ | GENERIC_WRITE, false, 3,
                             descriptor.data(), error, FILE_SHARE_READ | FILE_SHARE_WRITE);
        if (!file) io_error();
        check_private(file.value, false);
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        for (;;) {
            check_stop(stop);
            OVERLAPPED range{};
            if (::LockFileEx(file.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range)) break;
            if (::GetLastError() != ERROR_LOCK_VIOLATION) io_error();
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("credential lock wait timed out");
            pause(50ms, stop);
        }
    }
};

CredentialLock::CredentialLock(const CredentialPaths& paths, std::stop_token stop) {
    try { impl_ = std::make_unique<Impl>(paths, stop); }
    catch (const std::system_error&) { io_error(); }
}
CredentialLock::~CredentialLock() = default; // 先关闭锁文件、再关闭目录；关闭句柄释放范围锁。

void write_credentials(const CredentialPaths& paths, const Credentials& value) {
    try {
        Handle dir = directory(paths.file.parent_path(), true);
        DWORD error = ERROR_SUCCESS;
        Handle existing = open_relative(dir.value, paths.file.filename(), metadata_access, false, 1, nullptr, error);
        if (existing) check_private(existing.value, false);
        else if (error != ERROR_FILE_NOT_FOUND) io_error();
        auto descriptor = base::detail::private_security_descriptor();
        Temporary temp;
        temp.file = open_relative(dir.value, temporary_name(paths.file), FILE_WRITE_DATA | DELETE | metadata_access,
                                   false, 2, descriptor.data(), error);
        if (!temp.file) io_error();
        const auto data = credential_json(value).dump() + "\n";
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto size = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 64 << 10));
            DWORD written = 0;
            if (!::WriteFile(temp.file.value, data.data() + offset, size, &written, nullptr) || !written) io_error();
            offset += written;
        }
        if (!::FlushFileBuffers(temp.file.value)) io_error();
        rename_file(temp.file.value, dir.value, paths.file.filename());
        temp.published = true;
        ::FlushFileBuffers(dir.value); // 普通用户的目录句柄不保证支持 flush，不把不支持当成凭据失败。
    } catch (const std::system_error&) { io_error(); }
}

void remove_credentials(const CredentialPaths& paths) {
    try {
        Handle dir = directory(paths.file.parent_path(), false);
        if (!dir) io_error();
        DWORD error = ERROR_SUCCESS;
        Handle file = open_relative(dir.value, paths.file.filename(), DELETE | metadata_access, false, 1, nullptr, error);
        if (!file) io_error();
        check_private(file.value, false);
        // WDK FILE_DISPOSITION_INFORMATION_EX: DELETE=1, POSIX_SEMANTICS=2。
        // https://learn.microsoft.com/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_file_disposition_information_ex
        struct Disposition { DWORD flags; } disposition{0x00000001 | 0x00000002};
        if (!::SetFileInformationByHandle(file.value, FileDispositionInfoEx, &disposition, sizeof(disposition))) io_error();
        file = Handle{};
        ::FlushFileBuffers(dir.value);
    } catch (const std::system_error&) { io_error(); }
}
} // namespace detail

std::optional<Credentials> read_credentials(const CredentialPaths& paths) {
    try {
        Handle dir = directory(paths.file.parent_path(), false);
        if (!dir) return std::nullopt;
        DWORD error = ERROR_SUCCESS;
        Handle file = open_relative(dir.value, paths.file.filename(), FILE_READ_DATA | metadata_access, false, 1, nullptr, error);
        if (!file) { if (error == ERROR_FILE_NOT_FOUND) return std::nullopt; io_error(); }
        check_private(file.value, false);
        std::string data;
        char buffer[4096];
        for (;;) {
            DWORD bytes = 0;
            if (!::ReadFile(file.value, buffer, sizeof(buffer), &bytes, nullptr) || data.size() + bytes > 64 * 1024) io_error();
            if (!bytes) break;
            data.append(buffer, bytes);
        }
        try { return detail::decode_credentials(data); }
        catch (const nlohmann::json::exception&) { Credentials invalid; invalid.state = "invalid"; return invalid; }
    } catch (const std::system_error&) { io_error(); }
}

} // namespace dagent::llm
