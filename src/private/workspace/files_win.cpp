#include "workspace/files.hpp"
#include "workspace/files_detail.hpp"
#include "base/path.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>
#include <windows.h>
#include <winternl.h>
#include <aclapi.h>
#include <bcrypt.h>

// 用户态 SDK（winternl.h）不声明此 ntdll 入口。
extern "C" NTSYSAPI NTSTATUS NTAPI NtSetInformationFile(HANDLE handle, PIO_STATUS_BLOCK status, PVOID information,
                                                        ULONG length, FILE_INFORMATION_CLASS information_class);

namespace dagent::workspace {
namespace fs = std::filesystem;
using detail::fail;
namespace {

// NtCreateFile 的 WDK CreateDisposition/CreateOptions 值；不依赖 SDK 对这些宏的声明范围。
// https://learn.microsoft.com/windows/win32/api/winternl/nf-winternl-ntcreatefile
constexpr ULONG nt_open = 1, nt_create = 2, nt_open_if = 3;
constexpr ULONG nt_directory = 0x00000001, nt_non_directory = 0x00000040;
constexpr ULONG nt_synchronous = 0x00000020, nt_open_reparse = 0x00200000;
constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD metadata_access = FILE_READ_ATTRIBUTES | SYNCHRONIZE;

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~Handle() { reset(); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); value = std::exchange(other.value, INVALID_HANDLE_VALUE); }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
    void reset() {
        if (value != INVALID_HANDLE_VALUE) ::CloseHandle(std::exchange(value, INVALID_HANDLE_VALUE));
    }
};

std::error_code windows_error(DWORD error = ::GetLastError()) {
    return {static_cast<int>(error), std::system_category()};
}

bool missing(const std::error_code& error) {
    return error.value() == ERROR_FILE_NOT_FOUND || error.value() == ERROR_PATH_NOT_FOUND;
}

[[noreturn]] void io_error(std::string_view operation, const fs::path& path, const std::error_code& error) {
    fail(WorkspaceError::Kind::io, operation, path, error.message());
}

bool check_object(HANDLE handle, bool no_reparse, std::error_code& error) {
    if (::GetFileType(handle) != FILE_TYPE_DISK) { error = windows_error(ERROR_NOT_SUPPORTED); return false; }
    if (no_reparse) {
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
            error = windows_error();
            return false;
        }
        if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            error = windows_error(ERROR_CANT_ACCESS_FILE);
            return false;
        }
    }
    error.clear();
    return true;
}

Handle duplicate(HANDLE handle, std::error_code& error) {
    HANDLE result = INVALID_HANDLE_VALUE;
    if (!::DuplicateHandle(::GetCurrentProcess(), handle, ::GetCurrentProcess(), &result,
                            0, FALSE, DUPLICATE_SAME_ACCESS)) {
        error = windows_error();
        return Handle{};
    }
    error.clear();
    return Handle(result);
}

Handle reopen(HANDLE handle, DWORD access, std::error_code& error) {
    Handle result(::ReOpenFile(handle, access, sharing, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));
    if (!result) error = windows_error();
    else if (check_object(result.value, true, error)) return result;
    return Handle{};
}

// 每次只传一个分量，RootDirectory 始终是已经打开的目录；拒绝 ADS、上跳及 reparse point。
Handle open_component(HANDLE directory, const fs::path& component, DWORD access, ULONG options,
                      ULONG disposition, PSECURITY_DESCRIPTOR security, std::error_code& error) {
    const auto& name = component.native();
    if (name.empty() || name == L".." || name.find_first_of(L"\\/:") != std::wstring::npos ||
        name.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t)) {
        error = windows_error(ERROR_INVALID_NAME);
        return Handle{};
    }
    UNICODE_STRING text{};
    text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    text.MaximumLength = text.Length;
    text.Buffer = const_cast<wchar_t*>(name.data());
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = directory;
    attributes.ObjectName = &text;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    attributes.SecurityDescriptor = security;
    IO_STATUS_BLOCK status_block{};
    HANDLE raw = INVALID_HANDLE_VALUE;
    const ULONG file_attributes = options & nt_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    const NTSTATUS status = ::NtCreateFile(&raw, access, &attributes, &status_block, nullptr,
        file_attributes, sharing, disposition, options | nt_synchronous | nt_open_reparse, nullptr, 0);
    if (status < 0) {
        error = windows_error(::RtlNtStatusToDosError(status));
        return Handle{};
    }
    Handle result(raw);
    if (!check_object(raw, true, error)) return Handle{};
    return result;
}

// ReOpenFile 不能用于目录句柄（返回 ERROR_ACCESS_DENIED）；以空名字相对自身重新打开同一目录对象。
Handle reopen_directory(HANDLE directory, DWORD access, std::error_code& error) {
    UNICODE_STRING text{};
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = directory;
    attributes.ObjectName = &text;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK status_block{};
    HANDLE raw = INVALID_HANDLE_VALUE;
    const NTSTATUS status = ::NtCreateFile(&raw, access, &attributes, &status_block, nullptr, FILE_ATTRIBUTE_DIRECTORY,
        sharing, nt_open, nt_directory | nt_synchronous | nt_open_reparse, nullptr, 0);
    if (status < 0) {
        error = windows_error(::RtlNtStatusToDosError(status));
        return Handle{};
    }
    Handle result(raw);
    if (!check_object(raw, true, error)) return Handle{};
    return result;
}

Handle walk(HANDLE anchor, const fs::path& relative, DWORD access, bool directory, std::error_code& error) {
    Handle current = duplicate(anchor, error);
    if (!current) return current;
    std::vector<fs::path> components;
    for (const auto& component : relative)
        if (!component.empty() && component != L".") components.push_back(component);
    if (components.empty()) return directory ? reopen_directory(current.value, access, error) : reopen(current.value, access, error);
    for (std::size_t i = 0; i < components.size(); ++i) {
        const bool last = i + 1 == components.size();
        current = open_component(current.value, components[i], last ? access : metadata_access,
                                  !last || directory ? nt_directory : 0, nt_open, nullptr, error);
        if (!current) return current;
    }
    return current;
}

Handle open_absolute(const fs::path& path, DWORD access, bool directory, std::error_code& error) {
    if (!path.is_absolute()) { error = windows_error(ERROR_BAD_PATHNAME); return Handle{}; }
    Handle root(::CreateFileW(path.root_path().c_str(), metadata_access, sharing, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!root) { error = windows_error(); return Handle{}; }
    if (!check_object(root.value, true, error)) return Handle{};
    return walk(root.value, path.relative_path(), access, directory, error);
}

// 通用读取允许解析既有链接；审批后的 FileReference 始终使用上面的固定对象操作。
Handle open_path(const fs::path& path, DWORD access, std::error_code& error) {
    Handle result(::CreateFileW(path.c_str(), access, sharing, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!result) error = windows_error();
    else if (check_object(result.value, false, error)) return result;
    return Handle{};
}

struct Metadata {
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    FILE_ID_INFO id{};
};

Metadata metadata(HANDLE handle, std::string_view operation, const fs::path& path) {
    Metadata result;
    if (!::GetFileInformationByHandleEx(handle, FileBasicInfo, &result.basic, sizeof(result.basic)) ||
        !::GetFileInformationByHandleEx(handle, FileStandardInfo, &result.standard, sizeof(result.standard)) ||
        !::GetFileInformationByHandleEx(handle, FileIdInfo, &result.id, sizeof(result.id)))
        io_error(operation, path, windows_error());
    return result;
}

bool same_object(const FILE_ID_INFO& left, const FILE_ID_INFO& right) {
    return left.VolumeSerialNumber == right.VolumeSerialNumber &&
           std::memcmp(left.FileId.Identifier, right.FileId.Identifier, sizeof(left.FileId.Identifier)) == 0;
}

bool matches(HANDLE handle, const Metadata& expected) {
    FILE_ID_INFO id{};
    return ::GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) && same_object(id, expected.id);
}

bool regular(const Metadata& info) {
    return !(info.basic.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

Stamp make_stamp(const Metadata& info) {
    // FILETIME: 自 1601-01-01 起的 100 ns；clock_cast 兼容 MSVC file_clock 的 epoch/UTC 转换。
    using ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
    const auto sys = std::chrono::sys_time<ticks>(ticks(info.basic.LastWriteTime.QuadPart - 116444736000000000LL));
    Stamp stamp;
    stamp.mtime = std::chrono::time_point_cast<fs::file_time_type::duration>(
        std::chrono::clock_cast<fs::file_time_type::clock>(sys));
    stamp.size = static_cast<std::uintmax_t>(info.standard.EndOfFile.QuadPart);
    stamp.device = info.id.VolumeSerialNumber;
    std::memcpy(&stamp.inode, info.id.FileId.Identifier, sizeof(stamp.inode));
    std::memcpy(&stamp.inode_high, info.id.FileId.Identifier + sizeof(stamp.inode), sizeof(stamp.inode_high));
    return stamp;
}

TextFile read_handle(HANDLE handle, const fs::path& path, const FileOptions& options) {
    const auto info = metadata(handle, "read_text", path);
    if (info.standard.Directory) fail(WorkspaceError::Kind::io, "read_text", path, "is a directory");
    TextFile result;
    result.stamp = make_stamp(info);
    std::string raw;
    raw.reserve(std::min<std::size_t>(options.max_read_bytes, 1 << 20));
    std::array<char, 64 << 10> buffer{};
    while (raw.size() < options.max_read_bytes) {
        const auto want = static_cast<DWORD>(std::min(buffer.size(), options.max_read_bytes - raw.size()));
        DWORD bytes = 0;
        if (!::ReadFile(handle, buffer.data(), want, &bytes, nullptr)) {
            const auto error = windows_error();
            if (error.value() == ERROR_HANDLE_EOF) break;
            io_error("read_text", path, error);
        }
        if (!bytes) break;
        raw.append(buffer.data(), bytes);
    }
    if (raw.size() == options.max_read_bytes) {
        char extra = 0;
        DWORD bytes = 0;
        if (::ReadFile(handle, &extra, 1, &bytes, nullptr) && bytes) result.truncated = true;
    }
    detail::decode_text(raw, path, result);
    return result;
}

struct LocalFreeDeleter { void operator()(void* value) const { ::LocalFree(value); } };
struct Security {
    std::unique_ptr<void, LocalFreeDeleter> descriptor;
    PSID owner = nullptr, group = nullptr;
    PACL dacl = nullptr;
    SECURITY_INFORMATION flags = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
};

Security security_of(HANDLE handle, const fs::path& path) {
    std::error_code error;
    Handle readable = reopen(handle, READ_CONTROL | metadata_access, error);
    if (!readable) io_error("write security", path, error);
    Security result;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD code = ::GetSecurityInfo(readable.value, SE_FILE_OBJECT, result.flags,
                                          &result.owner, &result.group, &result.dacl, nullptr, &descriptor);
    if (code != ERROR_SUCCESS) io_error("write security", path, windows_error(code));
    result.descriptor.reset(descriptor);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!::GetSecurityDescriptorControl(descriptor, &control, &revision)) io_error("write security", path, windows_error());
    result.flags |= control & SE_DACL_PROTECTED ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
    return result;
}

std::wstring random_suffix(const fs::path& path) {
    std::array<UCHAR, 16> random{};
    const NTSTATUS status = ::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                                              BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) io_error("create temporary", path, windows_error(::RtlNtStatusToDosError(status)));
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    for (UCHAR byte : random) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

struct Temporary {
    Handle handle;
    bool keep = false;
    ~Temporary() {
        if (!keep && handle) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            ::SetFileInformationByHandle(handle.value, FileDispositionInfo, &disposition, sizeof(disposition));
        }
    }
};

void create_temporary(Temporary& temp, HANDLE parent, const std::wstring& prefix,
                      const Security& security, const fs::path& path) {
    const DWORD access = FILE_WRITE_DATA | DELETE | metadata_access |
                         (security.descriptor ? READ_CONTROL | WRITE_DAC | WRITE_OWNER : 0);
    std::error_code error;
    for (;;) {
        temp.handle = open_component(parent, fs::path(prefix + random_suffix(path)), access, nt_non_directory,
                                      nt_create, security.descriptor.get(), error);
        if (temp.handle) break;
        if (error.value() != ERROR_FILE_EXISTS && error.value() != ERROR_ALREADY_EXISTS)
            io_error("create temporary", path, error);
    }
    if (security.descriptor) {
        const DWORD code = ::SetSecurityInfo(temp.handle.value, SE_FILE_OBJECT, security.flags,
                                              security.owner, security.group, security.dacl, nullptr);
        if (code != ERROR_SUCCESS) io_error("write security", path, windows_error(code));
    }
}

void write_contents(HANDLE handle, std::string_view bytes, const fs::path& path) {
    while (!bytes.empty()) {
        DWORD written = 0;
        const auto size = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 64 << 10));
        if (!::WriteFile(handle, bytes.data(), size, &written, nullptr)) io_error("write", path, windows_error());
        if (!written) io_error("write", path, windows_error(ERROR_WRITE_FAULT));
        bytes.remove_prefix(written);
    }
    if (!::FlushFileBuffers(handle)) io_error("write", path, windows_error());
}

// 内核的 FILE_RENAME_INFORMATION（FileRenameInformationEx = 65）；Win32 的 SetFileInformationByHandle 不接受
// 带 RootDirectory 的相对重命名（返回 ERROR_INVALID_PARAMETER），所以直接调 NtSetInformationFile。
// https://learn.microsoft.com/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information
struct RenameInformationEx {
    ULONG Flags;
    HANDLE RootDirectory;
    ULONG FileNameLength;
    WCHAR FileName[1];
};

bool rename_into(HANDLE file, HANDLE parent, const fs::path& name, bool replace, std::error_code& error) {
    const auto& text = name.native();
    const auto bytes = text.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(RenameInformationEx) + bytes);
    auto* info = reinterpret_cast<RenameInformationEx*>(buffer.data());
    // FILE_RENAME_REPLACE_IF_EXISTS=1, FILE_RENAME_POSIX_SEMANTICS=2：后者允许旧目标仍被固定句柄持有。
    info->Flags = replace ? 0x00000001u | 0x00000002u : 0;
    info->RootDirectory = parent;
    info->FileNameLength = static_cast<ULONG>(bytes);
    std::memcpy(info->FileName, text.data(), bytes);
    IO_STATUS_BLOCK status_block{};
    const NTSTATUS status = ::NtSetInformationFile(file, &status_block, info, static_cast<ULONG>(buffer.size()),
                                                   static_cast<FILE_INFORMATION_CLASS>(65));
    if (status < 0) {
        error = windows_error(::RtlNtStatusToDosError(status));
        return false;
    }
    error.clear();
    return true;
}

Handle create_parents(HANDLE anchor, const fs::path& relative, const fs::path& path) {
    std::error_code error;
    Handle parent = duplicate(anchor, error);
    if (!parent) io_error("mkdir", path, error);
    for (const auto& component : relative) {
        if (component.empty() || component == L".") continue;
        parent = open_component(parent.value, component, metadata_access, nt_directory, nt_open_if, nullptr, error);
        if (!parent) {
            if (error.value() == ERROR_CANT_ACCESS_FILE || error.value() == ERROR_DIRECTORY)
                fail(WorkspaceError::Kind::stale, "write", path, "parent changed; prepare again: " + error.message());
            io_error("mkdir", path, error);
        }
    }
    return parent;
}

} // namespace

FileKind probe(const fs::path& path) {
    std::error_code error;
    Handle object = open_path(path, metadata_access, error);
    if (!object) {
        if (missing(error) || error.value() == ERROR_DIRECTORY) return FileKind::missing;
        io_error("probe", path, error);
    }
    const auto info = metadata(object.value, "probe", path);
    if (info.standard.Directory) return FileKind::directory;
    // 通用 API 允许既有链接，使用已打开的实际对象读取，不再按用户路径重开。
    Handle reader = reopen(object.value, FILE_READ_DATA | metadata_access, error);
    if (!reader) io_error("probe", path, error);
    char buffer[8 << 10];
    DWORD bytes = 0;
    if (!::ReadFile(reader.value, buffer, sizeof(buffer), &bytes, nullptr)) io_error("probe", path, windows_error());
    return std::memchr(buffer, '\0', bytes) ? FileKind::binary : FileKind::text;
}

TextFile read_text(const fs::path& path, const FileOptions& options) {
    std::error_code error;
    Handle handle = open_path(path, FILE_READ_DATA | metadata_access, error);
    if (!handle) io_error("read_text", path, error);
    return read_handle(handle.value, path, options);
}

std::optional<Stamp> stamp_of(const fs::path& path) {
    std::error_code error;
    Handle handle = open_path(path, metadata_access, error);
    if (!handle) {
        if (missing(error) || error.value() == ERROR_DIRECTORY) return std::nullopt;
        io_error("stat", path, error);
    }
    return make_stamp(metadata(handle.value, "stat", path));
}

void write_text(const fs::path& path, std::string_view content, Eol eol, bool bom,
                const std::optional<Stamp>& expect, const FileOptions& options) {
    if (content.size() > options.max_write_bytes)
        fail(WorkspaceError::Kind::too_large, "write_text", path, "content exceeds max_write_bytes");
    const fs::path target = detail::through_symlink(path);
    const auto directory = target.parent_path().empty() ? fs::path(L".") : target.parent_path();
    std::error_code error;
    // 通用入口允许原有父目录链接；先解析为实际路径，再固定最近祖先创建缺失目录。
    const auto canonical = fs::weakly_canonical(fs::absolute(directory), error);
    if (error) io_error("write_text", target, error);
    fs::path anchor_path = canonical;
    Handle anchor;
    for (;;) {
        anchor = open_absolute(anchor_path, metadata_access, true, error);
        if (anchor) break;
        if (!missing(error) || anchor_path == anchor_path.root_path()) io_error("write_text", target, error);
        anchor_path = anchor_path.parent_path();
    }
    Handle parent = create_parents(anchor.value, canonical.lexically_relative(anchor_path), target);
    const auto data = detail::encode_text(content, eol, bom);
    Handle existing = open_component(parent.value, target.filename(), metadata_access, 0, nt_open, nullptr, error);
    if (!existing && !missing(error)) io_error("write_text", target, error);
    std::optional<Metadata> previous;
    if (existing) previous = metadata(existing.value, "write_text", target);
    if (previous && previous->standard.Directory) fail(WorkspaceError::Kind::io, "write_text", target, "is a directory");
    if (expect && (!previous || make_stamp(*previous) != *expect))
        fail(WorkspaceError::Kind::stale, "write_text", target,
             previous ? "file changed since it was read" : "file disappeared");
    const auto security = existing ? security_of(existing.value, target) : Security{};
    Temporary temp;
    create_temporary(temp, parent.value, L"." + target.filename().native() + L".dagent-", security, target);
    write_contents(temp.handle.value, data, target);
    // 保留 DELETE 句柄供重命名，先关闭数据写句柄，使最后写入时间完成更新。
    Handle rename_handle = reopen(temp.handle.value, DELETE | metadata_access, error);
    if (!rename_handle) io_error("write_text", target, error);
    temp.handle = std::move(rename_handle);
    if (!rename_into(temp.handle.value, parent.value, target.filename(), true, error)) io_error("write_text", target, error);
    temp.keep = true;
    ::FlushFileBuffers(parent.value); // 目录 flush 是否支持由文件系统决定；与 POSIX 一样不作为提交失败。
}

struct FileReference::Impl {
    fs::path path, anchor_path, relative;
    Handle anchor, target;
    Metadata parent_info{}, target_info{};
};

FileReference::FileReference(const fs::path& path) : impl_(std::make_unique<Impl>()) {
    auto& r = *impl_;
    r.path = path;
    r.anchor_path = path == path.root_path() ? path : path.parent_path();
    std::error_code error;
    for (;;) {
        r.anchor = open_absolute(r.anchor_path, metadata_access, true, error);
        if (r.anchor) break;
        if (!missing(error) || r.anchor_path == r.anchor_path.root_path())
            fail(WorkspaceError::Kind::stale, "prepare", path, "parent changed; prepare again");
        r.anchor_path = r.anchor_path.parent_path();
    }
    r.parent_info = metadata(r.anchor.value, "prepare", path);
    r.relative = path.lexically_relative(r.anchor_path);
    r.target = walk(r.anchor.value, r.relative, metadata_access, false, error);
    if (!r.target && !missing(error)) fail(WorkspaceError::Kind::stale, "prepare", path, "target changed; prepare again");
    if (r.target) r.target_info = metadata(r.target.value, "prepare", path);
}

FileReference::~FileReference() = default;
FileReference::FileReference(FileReference&&) noexcept = default;
FileReference& FileReference::operator=(FileReference&&) noexcept = default;
FileKind FileReference::kind() const {
    if (!impl_->target) return FileKind::missing;
    return impl_->target_info.standard.Directory ? FileKind::directory : FileKind::text;
}
std::optional<Stamp> FileReference::stamp() const {
    return impl_->target ? std::optional(make_stamp(impl_->target_info)) : std::nullopt;
}
void FileReference::validate() const {
    const auto& r = *impl_;
    std::error_code error;
    Handle parent = open_absolute(r.anchor_path, metadata_access, true, error);
    if (!parent || !matches(parent.value, r.parent_info))
        fail(WorkspaceError::Kind::stale, "access", r.path, "parent changed; prepare again");
    Handle target = walk(r.anchor.value, r.relative, metadata_access, false, error);
    if (!r.target && !target && missing(error)) return;
    if (!r.target || !target || !matches(target.value, r.target_info))
        fail(WorkspaceError::Kind::stale, "access", r.path, "target changed; prepare again");
}
TextFile FileReference::read(const FileOptions& options) const {
    validate();
    const auto& r = *impl_;
    if (!r.target || !regular(r.target_info)) fail(WorkspaceError::Kind::io, "read", r.path, "not a regular file");
    std::error_code error;
    Handle reader = reopen(r.target.value, FILE_READ_DATA | metadata_access, error);
    if (!reader) io_error("read", r.path, error);
    return read_handle(reader.value, r.path, options);
}
void FileReference::erase() {
    validate();
    const auto& r = *impl_;
    if (!r.target || !regular(r.target_info)) fail(WorkspaceError::Kind::io, "delete", r.path, "not a regular file");
    std::error_code error;
    Handle parent = walk(r.anchor.value, r.relative.parent_path(), metadata_access, true, error);
    if (!parent) fail(WorkspaceError::Kind::stale, "delete", r.path, "parent changed; prepare again");
    Handle target = open_component(parent.value, r.relative.filename(), DELETE | metadata_access,
                                    nt_non_directory, nt_open, nullptr, error);
    if (!target) io_error("delete", r.path, error);
    if (!matches(target.value, r.target_info)) fail(WorkspaceError::Kind::stale, "delete", r.path, "target changed; prepare again");
    // WDK FILE_DISPOSITION_INFORMATION_EX（Flags 一项）；Win32 SDK 不一定声明对应结构/标志。
    // DELETE=1, POSIX_SEMANTICS=2：移除名称，已有固定句柄仍可访问旧对象。
    // https://learn.microsoft.com/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_file_disposition_information_ex
    struct DispositionInfoEx { DWORD flags; } disposition{0x00000001u | 0x00000002u};
    if (!::SetFileInformationByHandle(target.value, FileDispositionInfoEx, &disposition, sizeof(disposition)))
        io_error("delete", r.path, windows_error());
}

Stamp FileReference::write(std::string_view content, Eol eol, bool bom,
                           const std::optional<Stamp>& expect, const FileOptions& options) {
    validate();
    auto& r = *impl_;
    if (content.size() > options.max_write_bytes)
        fail(WorkspaceError::Kind::too_large, "write", r.path, "content exceeds max_write_bytes");
    if (r.target && !regular(r.target_info)) fail(WorkspaceError::Kind::io, "write", r.path, "not a regular file");
    Handle parent = create_parents(r.anchor.value, r.relative.parent_path(), r.path);
    const auto parent_info = metadata(parent.value, "write", r.path);
    if (expect && (!r.target || make_stamp(metadata(r.target.value, "write", r.path)) != *expect))
        fail(WorkspaceError::Kind::stale, "write", r.path, "file changed since read/preview");
    const auto data = detail::encode_reference_text(content, eol, bom);
    const auto security = r.target ? security_of(r.target.value, r.path) : Security{};
    Temporary temp;
    create_temporary(temp, parent.value, L".dagent-", security, r.path);
    write_contents(temp.handle.value, data, r.path);
    validate();
    std::error_code error;
    Handle current_parent = walk(r.anchor.value, r.relative.parent_path(), metadata_access, true, error);
    if (!current_parent) fail(WorkspaceError::Kind::stale, "write", r.path, "parent changed; prepare again: " + error.message());
    if (!matches(current_parent.value, parent_info))
        fail(WorkspaceError::Kind::stale, "write", r.path, "parent changed; prepare again: identity mismatch");
    if (expect && make_stamp(metadata(r.target.value, "write", r.path)) != *expect)
        fail(WorkspaceError::Kind::stale, "write", r.path, "file changed since preview");
    if (!rename_into(temp.handle.value, parent.value, r.relative.filename(), static_cast<bool>(r.target), error))
        fail(WorkspaceError::Kind::stale, "write", r.path, "target changed or cannot be replaced: " + error.message());
    temp.keep = true;
    ::FlushFileBuffers(parent.value);
    // 固定提交后的新对象，再关闭最后的数据写句柄，避免返回尚未更新的 LastWriteTime。
    Handle committed = reopen(temp.handle.value, metadata_access, error);
    if (!committed) io_error("write", r.path, error);
    temp.handle.reset();
    return make_stamp(metadata(committed.value, "write", r.path));
}

struct DirectoryEntries::Impl {
    Handle directory;
    fs::path path, current;
    std::vector<std::byte> buffer = std::vector<std::byte>(64 << 10);
    std::size_t next = 0;
    bool buffered = false, restart = true, ended = true;

    void advance(std::error_code& error) {
        error.clear();
        for (;;) {
            if (!buffered) {
                const auto type = restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
                if (!::GetFileInformationByHandleEx(directory.value, type, buffer.data(), static_cast<DWORD>(buffer.size()))) {
                    error = windows_error();
                    if (error.value() == ERROR_NO_MORE_FILES) error.clear();
                    ended = true;
                    return;
                }
                restart = false;
                next = 0;
            }
            const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + next);
            current = fs::path(std::wstring(entry->FileName, entry->FileNameLength / sizeof(wchar_t)));
            buffered = entry->NextEntryOffset != 0;
            next += entry->NextEntryOffset;
            if (current == L"." || current == L"..") continue;
            ended = false;
            return;
        }
    }
};

DirectoryEntries::DirectoryEntries(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DirectoryEntries::~DirectoryEntries() = default;
DirectoryEntries::DirectoryEntries(DirectoryEntries&&) noexcept = default;
DirectoryEntries& DirectoryEntries::operator=(DirectoryEntries&&) noexcept = default;
bool DirectoryEntries::at_end() const noexcept { return !impl_ || impl_->ended; }
fs::path DirectoryEntries::name() const { return impl_->current; }
bool DirectoryEntries::is_directory(std::error_code& error) const {
    Handle child = open_component(impl_->directory.value, impl_->current, metadata_access, 0, nt_open, nullptr, error);
    if (!child) return false;
    FILE_STANDARD_INFO info{};
    if (!::GetFileInformationByHandleEx(child.value, FileStandardInfo, &info, sizeof(info))) {
        error = windows_error();
        return false;
    }
    error.clear();
    return info.Directory != FALSE;
}
void DirectoryEntries::increment(std::error_code& error) { impl_->advance(error); }
void DirectoryEntries::increment() {
    std::error_code error;
    increment(error);
    if (error) throw fs::filesystem_error("directory iterator cannot advance", impl_->path, error);
}
DirectoryEntries FileReference::directory_entries(fs::directory_options options, std::error_code& error) const {
    validate();
    if (kind() != FileKind::directory) fail(WorkspaceError::Kind::io, "list", impl_->path, "not a directory");
    auto entries = std::make_unique<DirectoryEntries::Impl>();
    entries->path = impl_->path;
    entries->directory = reopen_directory(impl_->target.value, FILE_LIST_DIRECTORY | metadata_access, error);
    if (entries->directory) entries->advance(error);
    else if (error.value() == ERROR_ACCESS_DENIED && (options & fs::directory_options::skip_permission_denied) != fs::directory_options::none)
        error.clear();
    return DirectoryEntries(std::move(entries));
}
DirectoryEntries FileReference::directory_entries() const {
    std::error_code error;
    auto entries = directory_entries(fs::directory_options::none, error);
    if (error) throw fs::filesystem_error("directory iterator cannot open directory", impl_->path, error);
    return entries;
}

} // namespace dagent::workspace
