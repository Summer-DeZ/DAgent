/// @file configuration.hpp
/// @brief 装配侧配置网关：公开模型清单、模型添加与主题路径；凭据只留在内部配置值。
///
/// 实现 runtime::ConfigurationGateway；模型解析（按配置名重读配置并构造客户端）也在这里，
/// 供 SessionAssembly 切模型/子模型选择使用。
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agent/model_input.hpp"
#include "agent/port_model.hpp"
#include "agent/public_model.hpp"
#include "llm/provider.hpp"
#include "app/session_assembly.hpp"
#include "runtime/factory.hpp"

namespace dagent::app {

class Configuration final : public runtime::ConfigurationGateway {
public:
    Configuration(std::filesystem::path root,
                  std::vector<std::string> overrides, std::map<std::string, llm::ProviderConfig> models,
                  std::filesystem::path theme_file,
                  std::function<std::shared_ptr<agent::ModelSession>(const llm::ProviderConfig&)> make_session);

    std::vector<agent::PublicModel> models() const override;
    std::vector<agent::ProviderKindInfo> provider_kinds() const override;
    agent::PublicModel add_model(const agent::ModelInput& input) override;
    std::filesystem::path theme_file() const override;

    /// @brief 按配置名重读配置并构造客户端（@model 覆写）。凭据不进入公开返回值。
    ModelSelection resolve(const std::string& name);

private:
    std::filesystem::path root_, theme_file_;
    std::vector<std::string> overrides_;
    std::map<std::string, llm::ProviderConfig> models_;
    std::function<std::shared_ptr<agent::ModelSession>(const llm::ProviderConfig&)> make_session_;
};

} // namespace dagent::app
