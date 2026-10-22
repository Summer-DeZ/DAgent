#include "app/sandbox_status.hpp"

namespace dagent::app::detail {

// Windows 第一版没有沙箱后端：不存在 bwrap/socat 对应物，也没有 AppArmor 一类的宿主诊断。
bool platform_sandbox_supported() { return false; }
std::optional<std::filesystem::path> host_bwrap() { return std::nullopt; }
std::optional<std::filesystem::path> host_socat() { return std::nullopt; }
void append_host_diagnostics(nlohmann::json&) {}

} // namespace dagent::app::detail
