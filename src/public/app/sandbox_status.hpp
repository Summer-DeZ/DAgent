/// @file sandbox_status.hpp
/// @brief 沙箱后端诊断：静态依赖清单与一次真实隔离启动分开报告。
///
/// 只调用生产后端（internal/sandbox 环境 + SRT bridge），不提供模拟检测入口。
/// 静态项齐全不等于隔离可用；`sandbox_probe` 是真实启动结果，失败时必须给出阶段与宿主前置。
#pragma once

#include <filesystem>
#include <optional>

#include "app/home.hpp"
#include "exec/srt.hpp"
#include "lib/nlohmann/json.hpp"

namespace dagent::app {

/// @brief 解析 SRT 后端资源；runtime 未同步或任一依赖缺失时返回 nullopt。
std::optional<exec::SrtRuntime> sandbox_runtime(const HomePaths& paths);

/// @brief 真实做一次最小隔离启动（初始化 SRT、bwrap、写私有目录、跑命令）。失败返回 ok=false。
nlohmann::json sandbox_probe(const exec::SrtRuntime& runtime, const std::filesystem::path& state_root);

/// @brief `dagent sandbox status` 的结果。运行前不要求 runtime 已同步。
nlohmann::json sandbox_status(const HomePaths& paths);

} // namespace dagent::app
