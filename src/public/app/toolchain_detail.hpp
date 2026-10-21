/// @file toolchain_detail.hpp
/// @brief 托管运行时的平台操作、共享命令执行与安装锁，仅供 app 内部使用。
#pragma once

#include <filesystem>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/home.hpp"
#include "exec/process.hpp"
#include "lib/nlohmann/json.hpp"

namespace dagent::app::detail {

std::string runtime_platform();

/// @brief 从空环境启动准备命令；共享实现保留超时、输出上限、错误和尾部换行处理。
std::string execute_runtime_command(
    std::vector<std::string> argv,
    const std::filesystem::path& cwd,
    const std::vector<std::pair<std::string, std::string>>& env = {});

/// @brief 解压已经下载并验证摘要的归档；下载和校验仍由共享层负责。
void extract_runtime_archive(const std::filesystem::path& archive,
                             const std::filesystem::path& destination,
                             const std::filesystem::path& cwd,
                             int strip);

exec::Environment runtime_environment(const HomePaths& paths, const nlohmann::json& installed,
                                      std::string_view name);

/// @brief 准备 Python 包及程序入口；cache 为 HomePaths::cache，POSIX 使用其 uv 子目录。
using RuntimePrograms = std::map<std::string, std::filesystem::path>;
RuntimePrograms install_python_runtime(const std::filesystem::path& uv, std::string_view version,
                            const std::filesystem::path& directory, const std::filesystem::path& cache);

/// @brief POSIX 保留配置声明的程序路径；Windows 使用安装器发现的原生解释器路径。
std::filesystem::path select_python_runtime_program(const std::filesystem::path& configured,
                                                    const std::filesystem::path& installed);

std::filesystem::path python_environment_program(const std::filesystem::path& environment_root);

class SyncLock {
public:
    explicit SyncLock(const std::filesystem::path& path);
    ~SyncLock();
    SyncLock(const SyncLock&) = delete;
    SyncLock& operator=(const SyncLock&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dagent::app::detail
