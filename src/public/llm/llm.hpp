/// @file llm.hpp
/// @brief llm 模块的装配入口：内部配置 ↔ 公开描述的映射、模型客户端工厂。
#pragma once

#include <memory>

#include "agent/public_model.hpp"
#include "llm/model.hpp"

namespace dagent::llm {

/// @brief 内部配置的公开视图：复制展示与预算字段，绝不带出凭据。
agent::PublicModel to_public(const ProviderConfig&);

} // namespace dagent::llm
