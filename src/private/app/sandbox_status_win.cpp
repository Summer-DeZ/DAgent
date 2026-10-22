#include "app/sandbox_status.hpp"

namespace dagent::app::detail {

// Windows 暂不启用沙箱后端（ProcessContainer 接入在后续阶段）；没有 Linux 宿主前置，也没有 AppArmor 一类的宿主诊断。
bool platform_sandbox_supported() { return false; }
std::string_view sandbox_containment() { return "processcontainer"; }
std::vector<HostCheck> host_prerequisites() { return {}; }
void append_host_diagnostics(nlohmann::json&) {}

} // namespace dagent::app::detail
