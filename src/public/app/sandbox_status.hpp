/// @file sandbox_status.hpp
/// @brief 沙箱后端诊断：静态依赖清单与一次真实隔离启动分开报告。
///
/// 只调用生产后端（runtime 登记的 MXC 原生库 + exec::run_mxc），不提供模拟检测入口。
/// 静态项齐全不等于隔离可用；`sandbox_probe` 是真实启动结果，失败时必须给出阶段与宿主前置。
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "app/home.hpp"
#include "exec/mxc.hpp"
#include "lib/nlohmann/json.hpp"

namespace dagent::app {

/// @brief 解析 MXC 后端资源：runtime 已同步、原生库可加载且版本为 kMxcVersion、managed bash/rg 可执行。
/// 任一条件不满足时返回原因。
std::expected<exec::MxcRuntime, std::string> sandbox_runtime(const HomePaths& paths);

/// @brief 真实做两次最小隔离启动：只读模式下在私有 HOME 写入并读回标记；再以拒绝一切的网络闸门
/// 启用代理网络运行 `true`（经过 slirp4netns 与命名空间内防火墙）。失败返回 ok=false 与 stage/error。
nlohmann::json sandbox_probe(const exec::MxcRuntime& runtime);

/// @brief `dagent sandbox status` 的结果。运行前不要求 runtime 已同步。
nlohmann::json sandbox_status(const HomePaths& paths);

namespace detail {
/// 宿主前置程序的一项检查。
struct HostCheck {
    std::string name;
    bool ok = false;
    std::string detail;
};

/// @brief 本平台是否提供沙箱后端；为 false 时不解析 MXC 资源、不做启动探测，状态报告 `unsupported`。
bool platform_sandbox_supported();
/// @brief MxcRuntime::containment 的平台取值。
std::string_view sandbox_containment();
/// @brief MXC 后端在宿主上依赖的程序；Linux 为 bwrap（≥ 0.5）、slirp4netns、unshare、nsenter、ip、iptables-restore。
std::vector<HostCheck> host_prerequisites();
void append_host_diagnostics(nlohmann::json& out);
} // namespace detail

} // namespace dagent::app
