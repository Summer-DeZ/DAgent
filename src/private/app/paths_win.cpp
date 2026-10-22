#include "app/paths.hpp"
#include "base/path.hpp"
#include "base/platform.hpp"

#include <format>
#include <windows.h>

namespace dagent::app::detail {
namespace fs = std::filesystem;

fs::path executable_home_directory() {
    std::error_code error;
    auto executable = base::executable_path(error);
    if (error) throw InstallError(std::format("cannot resolve executable path: {}", error.message()));
    return executable.parent_path();
}

void probe_home_writable(const fs::path& root) {
    const fs::path probe = root / std::format(".dagent-write-{}", base::process_id());
    const HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const std::error_code error(static_cast<int>(::GetLastError()), std::system_category());
        throw InstallError(std::format(
            "installation root {} is not writable (sessions and logs are stored there): {}. Set DAGENT_HOME to a writable directory",
            base::path_to_utf8(root), error.message()));
    }
    ::CloseHandle(file);
    ::DeleteFileW(probe.c_str());
}

} // namespace dagent::app::detail
