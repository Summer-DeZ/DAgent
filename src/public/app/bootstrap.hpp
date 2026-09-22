/// @file bootstrap.hpp
/// @brief 前端/后端起点的共享装配：模型工厂、权限初值与 Setup 构造。
///
/// 后端在 app.initialize 时用同一入口装配，保证 in-process 与协议路径的会话初值一致。
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/setup.hpp"
#include "app/assembly.hpp"
#include "app/config.hpp"

namespace dagent::app {

struct BootstrapParams {
    std::filesystem::path root;
    std::filesystem::path cwd;
    std::vector<std::string> overrides;
    std::optional<std::string> permissions; ///< --permissions；空表示用 config.json
    bool read_only = false;
    bool plan = false;
};

/// @brief 进程内共享的模型客户端工厂：HTTP 调整与重试参数来自配置。
std::function<std::shared_ptr<agent::ModelSession>(const llm::ProviderConfig&)>
make_model_factory(const Config& config);

agent::PermissionMode permission_mode(const Config& config, const BootstrapParams& params);

/// @brief 从配置与启动参数构造会话装配输入（cwd/工具/存储/MCP/提示词/沙箱探测）。
agent::Setup make_setup(const Config& config, const BootstrapParams& params,
                        const std::shared_ptr<Assembly>& assembly);

} // namespace dagent::app
