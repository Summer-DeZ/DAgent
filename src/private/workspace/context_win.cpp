#include "workspace/context.hpp"

#include <format>
#include <windows.h>

// 用户态 SDK 不声明此 ntdll 入口；它返回不受应用兼容性 manifest 影响的版本。
extern "C" NTSYSAPI LONG NTAPI RtlGetVersion(OSVERSIONINFOW* version);

namespace dagent::workspace::detail {

std::string os_description() {
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (::RtlGetVersion(&version) < 0) return "unknown";
    return std::format("Windows {}.{}.{}", version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
}

std::string shell_description() { return "Git Bash"; }

} // namespace dagent::workspace::detail
