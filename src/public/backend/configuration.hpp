#pragma once

#include "runtime/factory.hpp"

namespace dagent::backend {
// 前端外观属于应用配置；不扩展 runtime 的模型配置端口。
class ConfigurationGateway : public runtime::ConfigurationGateway {
public:
    virtual nlohmann::json layout() const = 0;
    virtual nlohmann::json appearance() const = 0;
    virtual std::filesystem::path themes_directory() const = 0;
    virtual nlohmann::json save_appearance(const nlohmann::json&, const nlohmann::json&) = 0;
};
} // namespace dagent::backend
