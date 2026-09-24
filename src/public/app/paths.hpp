/// @file paths.hpp
/// @brief 安装根解析：DAGENT_HOME、dev 默认 home 或可执行文件所在目录。
///
/// 前端解析后把 root 交给后端 initialize；后端不自行决定安装根。
#pragma once

#include <filesystem>
#include <stdexcept>

namespace dagent::app {

struct InstallationPaths {
    std::filesystem::path root;
    std::filesystem::path config;
    std::filesystem::path models;
    std::filesystem::path database;
    std::filesystem::path logs;
};

/// @brief 安装根不可用（不存在/不可写）时抛出；前端按配置错误返回退出码 2。
class InstallError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief DAGENT_HOME、dev 默认 home 或 /proc/self/exe 的父目录；同时验证根存在、是目录且可写。
InstallationPaths installation_paths();

} // namespace dagent::app
