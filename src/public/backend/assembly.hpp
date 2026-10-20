/// @file assembly.hpp
/// @brief backend 启动与装配契约，由 app 提供具体实现。
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "backend/configuration.hpp"
#include "backend/login.hpp"
#include "backend/queries.hpp"
#include "runtime/factory.hpp"

namespace dagent::backend {

/// @brief 后端启动输入：只在装配层读取。
struct BootstrapOptions {
    std::string mode; ///< interactive / run / sessions / models
    std::filesystem::path root;
    std::filesystem::path cwd;
    std::vector<std::string> overrides;     ///< 按序 --set
    std::optional<std::string> permissions; ///< --permissions；空表示用 config.json
    bool read_only = false;
    bool plan = false;
    std::optional<std::string> log_level;
};

/// @brief 装配结果：backend 只经这些端口访问具体实现。
struct Assembled {
    nlohmann::json maintenance;
    std::shared_ptr<LoginGateway> login;
    std::shared_ptr<ConfigurationGateway> configuration;
    std::shared_ptr<QueryGateway> queries;
    std::unique_ptr<runtime::SessionFactory> factory; ///< 纯查询模式（sessions/models）为空
    std::string default_model;
    int progress_interval_ms = 1000;
};

/// @brief 配置文件或覆写无效；backend 映射为协议 config_error。
class ConfigurationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief 读取配置、初始化日志并装配具体对象；由 app 装配层实现并注入 backend。
using Assembler = std::function<Assembled(const BootstrapOptions&)>;

} // namespace dagent::backend
