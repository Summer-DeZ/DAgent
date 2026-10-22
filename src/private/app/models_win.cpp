#include "app/config_detail.hpp"
#include "app/home.hpp"
#include "base/path.hpp"
#include "base/security_detail.hpp"

#include <format>
#include <system_error>
#include <windows.h>

namespace dagent::app::detail {
namespace fs = std::filesystem;

struct ModelWriteLock::Impl {
    HANDLE file = INVALID_HANDLE_VALUE;
    bool locked = false;
    ~Impl() {
        if (file != INVALID_HANDLE_VALUE) {
            if (locked) { OVERLAPPED range{}; ::UnlockFileEx(file, 0, 1, 0, &range); }
            ::CloseHandle(file);
        }
    }
};

ModelWriteLock::ModelWriteLock(const fs::path& root) : impl_(std::make_unique<Impl>()) {
    const fs::path path = HomePaths(root).run / "models.lock";
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) fail(ConfigError::Kind::io, "cannot create the model lock directory: " + error.message());
    try {
        auto descriptor = base::detail::private_security_descriptor();
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.data(), FALSE};
        impl_->file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &attributes, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (impl_->file == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category());
    } catch (const std::system_error& exception) {
        fail(ConfigError::Kind::io, std::format("cannot open {}: {}", base::path_to_utf8(path), exception.what()));
    }
    // 同步句柄且不带 FAIL_IMMEDIATELY，LockFileEx 在这里阻塞等待。
    OVERLAPPED range{};
    if (!::LockFileEx(impl_->file, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &range))
        fail(ConfigError::Kind::io, std::format("cannot lock {}: {}", base::path_to_utf8(path),
             std::system_category().message(static_cast<int>(::GetLastError()))));
    impl_->locked = true;
}
ModelWriteLock::~ModelWriteLock() = default;

} // namespace dagent::app::detail
