/// @file public_model.hpp
/// @brief 模型的公开描述：不含密钥与会话状态，装配层、核心与前端展示共用。
///
/// 密钥、extra_body 等适配内部配置只存在于 llm::ProviderConfig（llm/provider.hpp）。
#pragma once

#include <cstddef>
#include <string>

namespace dagent::agent {

struct PublicModel {
    std::string name;   ///< models.json 里的配置名（切换目标）
    std::string kind;   ///< provider 种类，如 openai-chat
    std::string model;  ///< 发给 provider 的模型 ID
    std::string base_url;
    std::size_t max_tokens = 0;
    double temperature = -1.0;
    std::size_t context_window = 0;
    bool has_key = false; ///< 是否配置了凭据；不含凭据本身
};

} // namespace dagent::agent
