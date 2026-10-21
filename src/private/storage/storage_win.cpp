#include "storage/platform.hpp"
#include "storage/sqlite.hpp"
#include "base/path.hpp"
#include "base/security_detail.hpp"

#include <format>
#include <system_error>
#include <windows.h>

namespace dagent::storage::detail {

struct SessionFileLock::Impl {
    HANDLE file = INVALID_HANDLE_VALUE;
    bool locked = false;
    ~Impl() {
        if (file != INVALID_HANDLE_VALUE) {
            if (locked) { OVERLAPPED range{}; ::UnlockFileEx(file, 0, 1, 0, &range); }
            ::CloseHandle(file);
        }
    }
};

SessionFileLock::SessionFileLock(const std::filesystem::path& path, std::string_view session_id)
    : impl_(std::make_unique<Impl>()) {
    try {
        auto descriptor = base::detail::private_security_descriptor();
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.data(), FALSE};
        impl_->file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &attributes, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (impl_->file == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category());
    } catch (const std::system_error& error) {
        fail(StorageError::Kind::io, std::format("cannot open session lock {}: {}", base::path_to_utf8(path), error.what()));
    }
    OVERLAPPED range{};
    if (!::LockFileEx(impl_->file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range))
        fail(StorageError::Kind::io, std::format("session {} is already in use by another process", session_id));
    impl_->locked = true;
}

SessionFileLock::~SessionFileLock() = default;

} // namespace dagent::storage::detail
