#include "app/config_detail.hpp"
#include "base/path.hpp"
#include "base/security_detail.hpp"

#include <memory>
#include <system_error>
#include <windows.h>
#include <aclapi.h>

namespace dagent::app::detail {
namespace {
struct CloseHandleDeleter { void operator()(void* handle) const { ::CloseHandle(handle); } };
struct LocalFreeDeleter { void operator()(void* memory) const { ::LocalFree(memory); } };
}

void require_private_file(const std::filesystem::path& file) {
    const auto path = base::path_to_utf8(file);
    HANDLE raw = ::CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE)
        fail(ConfigError::Kind::io, std::format("cannot inspect {}: {}", path,
             std::system_category().message(static_cast<int>(::GetLastError()))));
    std::unique_ptr<void, CloseHandleDeleter> handle(raw);
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!::GetFileInformationByHandleEx(raw, FileAttributeTagInfo, &info, sizeof(info)))
        fail(ConfigError::Kind::io, std::format("cannot inspect {}: {}", path,
             std::system_category().message(static_cast<int>(::GetLastError()))));
    if (::GetFileType(raw) != FILE_TYPE_DISK || info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        fail(ConfigError::Kind::io, path + " is not a regular file");
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD code = ::GetSecurityInfo(raw, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                          nullptr, nullptr, nullptr, nullptr, &descriptor);
    if (code != ERROR_SUCCESS)
        fail(ConfigError::Kind::io, std::format("cannot inspect {}: {}", path, std::system_category().message(static_cast<int>(code))));
    std::unique_ptr<void, LocalFreeDeleter> security(descriptor);
    bool private_file = false;
    try {
        private_file = base::detail::is_private_security_descriptor(
            {static_cast<const std::byte*>(descriptor), ::GetSecurityDescriptorLength(descriptor)}, false, false);
    } catch (const std::system_error& exception) {
        fail(ConfigError::Kind::io, std::format("cannot inspect {}: {}", path, exception.what()));
    }
    if (!private_file)
        fail(ConfigError::Kind::invalid, path +
             " must have a private Windows DACL because it may contain API keys; in Security > Advanced, set the owner to the current user, disable inheritance "
             "and grant access only to the current user, SYSTEM and Administrators (the current user needs read/write access)");
}

} // namespace dagent::app::detail
