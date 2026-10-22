#include "app/sandbox_status.hpp"

#include <string>

namespace dagent::app::detail {

// Windows 的后端是 MXC ProcessContainer（只接受 base-container 层，由共享代码按请求探测判断）；
// 没有 Linux 那样的宿主前置程序。
bool platform_sandbox_supported() { return true; }
std::string_view sandbox_containment() { return "processcontainer"; }
std::vector<HostCheck> host_prerequisites() { return {}; }

void append_host_diagnostics(nlohmann::json& out) {
    // Git bash 的 MSYS2 运行库用绝对 NT 路径在全局 \BaseNamedObjects 下建共享目录，容器进程没有该权限，
    // 任何策略都无法放行（microsoft/mxc#1061）。
    const std::string error = out["probe"].value("error", "");
    if (!out["probe"].value("ok", false) && error.find("NtCreateDirectoryObject") != std::string::npos)
        out["host_prerequisite"] =
            "Git Bash (MSYS2) cannot initialize inside a ProcessContainer: msys-2.0.dll creates its shared objects "
            "under the global \\BaseNamedObjects, which container processes may not write (microsoft/mxc#1061)";
}

} // namespace dagent::app::detail
