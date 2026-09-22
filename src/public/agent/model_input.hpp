/// @file model_input.hpp
/// @brief 模型表单的中立输入值：前端逐项收集并校验，后端解析、保存并构造客户端。
///
/// credential 是仅此次请求可写的字段（明文或 env:KEY 形式），不进入公开描述，
/// 也不被核心或前端保存；对应 llm::ProviderConfig::api_key 只在装配层转换。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dagent::agent {

struct ModelInput {
    std::string kind;                  ///< provider 种类，如 openai-chat
    std::string name;                  ///< models.json 里的配置名
    std::string base_url;
    std::string model;                 ///< 发给 provider 的模型 ID
    std::string credential;            ///< 仅此次请求可写；空表示无凭据
    std::size_t max_tokens = 8192;
    std::size_t context_window = 0;
};

/// @brief provider 种类的公开元数据：表单校验与默认值用，不含任何配置内容。
struct ProviderKindInfo {
    std::string kind;
    std::string default_base_url;
    bool needs_credential = false;
};

} // namespace dagent::agent
